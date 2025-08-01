// SPDX-License-Identifier: GPL-2.0
/*
 *  Shared Memory Communications over RDMA (SMC-R) and RoCE
 *
 *  Basic Transport Functions exploiting Infiniband API
 *
 *  Copyright IBM Corp. 2016
 *
 *  Author(s):  Ursula Braun <ubraun@linux.vnet.ibm.com>
 */

#include <linux/socket.h>
#include <linux/if_vlan.h>
#include <linux/random.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/reboot.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/smc.h>
#include <net/tcp.h>
#include <net/sock.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_cache.h>

#include "smc.h"
#include "smc_clc.h"
#include "smc_core.h"
#include "smc_ib.h"
#include "smc_wr.h"
#include "smc_llc.h"
#include "smc_cdc.h"
#include "smc_close.h"
#include "smc_ism.h"
#include "smc_netlink.h"
#include "smc_stats.h"
#include "smc_tracepoint.h"

#define SMC_LGR_NUM_INCR		256
#define SMC_LGR_FREE_DELAY_SERV		(600 * HZ)
#define SMC_LGR_FREE_DELAY_CLNT		(SMC_LGR_FREE_DELAY_SERV + 10 * HZ)

struct smc_lgr_list smc_lgr_list = {	/* established link groups */
	.lock = __SPIN_LOCK_UNLOCKED(smc_lgr_list.lock),
	.list = LIST_HEAD_INIT(smc_lgr_list.list),
	.num = 0,
};

static atomic_t lgr_cnt = ATOMIC_INIT(0); /* number of existing link groups */
static DECLARE_WAIT_QUEUE_HEAD(lgrs_deleted);

static void smc_buf_free(struct smc_link_group *lgr, bool is_rmb,
			 struct smc_buf_desc *buf_desc);
static void __smc_lgr_terminate(struct smc_link_group *lgr, bool soft);

static void smc_link_down_work(struct work_struct *work);

/* return head of link group list and its lock for a given link group */
static inline struct list_head *smc_lgr_list_head(struct smc_link_group *lgr,
						  spinlock_t **lgr_lock)
{
	if (lgr->is_smcd) {
		*lgr_lock = &lgr->smcd->lgr_lock;
		return &lgr->smcd->lgr_list;
	}

	*lgr_lock = &smc_lgr_list.lock;
	return &smc_lgr_list.list;
}

static void smc_ibdev_cnt_inc(struct smc_link *lnk)
{
	atomic_inc(&lnk->smcibdev->lnk_cnt_by_port[lnk->ibport - 1]);
}

static void smc_ibdev_cnt_dec(struct smc_link *lnk)
{
	atomic_dec(&lnk->smcibdev->lnk_cnt_by_port[lnk->ibport - 1]);
}

static void smc_lgr_schedule_free_work(struct smc_link_group *lgr)
{
	/* client link group creation always follows the server link group
	 * creation. For client use a somewhat higher removal delay time,
	 * otherwise there is a risk of out-of-sync link groups.
	 */
	if (!lgr->freeing) {
		mod_delayed_work(system_wq, &lgr->free_work,
				 (!lgr->is_smcd && lgr->role == SMC_CLNT) ?
						SMC_LGR_FREE_DELAY_CLNT :
						SMC_LGR_FREE_DELAY_SERV);
	}
}

/* Register connection's alert token in our lookup structure.
 * To use rbtrees we have to implement our own insert core.
 * Requires @conns_lock
 * @smc		connection to register
 * Returns 0 on success, != otherwise.
 */
static void smc_lgr_add_alert_token(struct smc_connection *conn)
{
	struct rb_node **link, *parent = NULL;
	u32 token = conn->alert_token_local;

	link = &conn->lgr->conns_all.rb_node;
	while (*link) {
		struct smc_connection *cur = rb_entry(*link,
					struct smc_connection, alert_node);

		parent = *link;
		if (cur->alert_token_local > token)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	/* Put the new node there */
	rb_link_node(&conn->alert_node, parent, link);
	rb_insert_color(&conn->alert_node, &conn->lgr->conns_all);
}

/* assign an SMC-R link to the connection */
static int smcr_lgr_conn_assign_link(struct smc_connection *conn, bool first)
{
	enum smc_link_state expected = first ? SMC_LNK_ACTIVATING :
				       SMC_LNK_ACTIVE;
	int i, j;

	/* do link balancing */
	conn->lnk = NULL;	/* reset conn->lnk first */
	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		struct smc_link *lnk = &conn->lgr->lnk[i];

		if (lnk->state != expected || lnk->link_is_asym)
			continue;
		if (conn->lgr->role == SMC_CLNT) {
			conn->lnk = lnk; /* temporary, SMC server assigns link*/
			break;
		}
		if (conn->lgr->conns_num % 2) {
			for (j = i + 1; j < SMC_LINKS_PER_LGR_MAX; j++) {
				struct smc_link *lnk2;

				lnk2 = &conn->lgr->lnk[j];
				if (lnk2->state == expected &&
				    !lnk2->link_is_asym) {
					conn->lnk = lnk2;
					break;
				}
			}
		}
		if (!conn->lnk)
			conn->lnk = lnk;
		break;
	}
	if (!conn->lnk)
		return SMC_CLC_DECL_NOACTLINK;
	atomic_inc(&conn->lnk->conn_cnt);
	return 0;
}

/* Register connection in link group by assigning an alert token
 * registered in a search tree.
 * Requires @conns_lock
 * Note that '0' is a reserved value and not assigned.
 */
static int smc_lgr_register_conn(struct smc_connection *conn, bool first)
{
	struct smc_sock *smc = container_of(conn, struct smc_sock, conn);
	static atomic_t nexttoken = ATOMIC_INIT(0);
	int rc;

	if (!conn->lgr->is_smcd) {
		rc = smcr_lgr_conn_assign_link(conn, first);
		if (rc) {
			conn->lgr = NULL;
			return rc;
		}
	}
	/* find a new alert_token_local value not yet used by some connection
	 * in this link group
	 */
	sock_hold(&smc->sk); /* sock_put in smc_lgr_unregister_conn() */
	while (!conn->alert_token_local) {
		conn->alert_token_local = atomic_inc_return(&nexttoken);
		if (smc_lgr_find_conn(conn->alert_token_local, conn->lgr))
			conn->alert_token_local = 0;
	}
	smc_lgr_add_alert_token(conn);
	conn->lgr->conns_num++;
	return 0;
}

/* Unregister connection and reset the alert token of the given connection<
 */
static void __smc_lgr_unregister_conn(struct smc_connection *conn)
{
	struct smc_sock *smc = container_of(conn, struct smc_sock, conn);
	struct smc_link_group *lgr = conn->lgr;

	rb_erase(&conn->alert_node, &lgr->conns_all);
	if (conn->lnk)
		atomic_dec(&conn->lnk->conn_cnt);
	lgr->conns_num--;
	conn->alert_token_local = 0;
	sock_put(&smc->sk); /* sock_hold in smc_lgr_register_conn() */
}

/* Unregister connection from lgr
 */
static void smc_lgr_unregister_conn(struct smc_connection *conn)
{
	struct smc_link_group *lgr = conn->lgr;

	if (!smc_conn_lgr_valid(conn))
		return;
	write_lock_bh(&lgr->conns_lock);
	if (conn->alert_token_local) {
		__smc_lgr_unregister_conn(conn);
	}
	write_unlock_bh(&lgr->conns_lock);
}

static void smc_lgr_buf_list_add(struct smc_link_group *lgr,
				 bool is_rmb,
				 struct list_head *buf_list,
				 struct smc_buf_desc *buf_desc)
{
	list_add(&buf_desc->list, buf_list);
	if (is_rmb) {
		lgr->alloc_rmbs += buf_desc->len;
		lgr->alloc_rmbs +=
			lgr->is_smcd ? sizeof(struct smcd_cdc_msg) : 0;
	} else {
		lgr->alloc_sndbufs += buf_desc->len;
	}
}

static void smc_lgr_buf_list_del(struct smc_link_group *lgr,
				 bool is_rmb,
				 struct smc_buf_desc *buf_desc)
{
	list_del(&buf_desc->list);
	if (is_rmb) {
		lgr->alloc_rmbs -= buf_desc->len;
		lgr->alloc_rmbs -=
			lgr->is_smcd ? sizeof(struct smcd_cdc_msg) : 0;
	} else {
		lgr->alloc_sndbufs -= buf_desc->len;
	}
}

int smc_nl_get_sys_info(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct smc_nl_dmp_ctx *cb_ctx = smc_nl_dmp_ctx(cb);
	char hostname[SMC_MAX_HOSTNAME_LEN + 1];
	char smc_seid[SMC_MAX_EID_LEN + 1];
	struct nlattr *attrs;
	u8 *seid = NULL;
	u8 *host = NULL;
	void *nlh;

	nlh = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &smc_gen_nl_family, NLM_F_MULTI,
			  SMC_NETLINK_GET_SYS_INFO);
	if (!nlh)
		goto errmsg;
	if (cb_ctx->pos[0])
		goto errout;
	attrs = nla_nest_start(skb, SMC_GEN_SYS_INFO);
	if (!attrs)
		goto errout;
	if (nla_put_u8(skb, SMC_NLA_SYS_VER, SMC_V2))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_SYS_REL, SMC_RELEASE))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_SYS_IS_ISM_V2, smc_ism_is_v2_capable()))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_SYS_IS_SMCR_V2, true))
		goto errattr;
	smc_clc_get_hostname(&host);
	if (host) {
		memcpy(hostname, host, SMC_MAX_HOSTNAME_LEN);
		hostname[SMC_MAX_HOSTNAME_LEN] = 0;
		if (nla_put_string(skb, SMC_NLA_SYS_LOCAL_HOST, hostname))
			goto errattr;
	}
	if (smc_ism_is_v2_capable()) {
		smc_ism_get_system_eid(&seid);
		memcpy(smc_seid, seid, SMC_MAX_EID_LEN);
		smc_seid[SMC_MAX_EID_LEN] = 0;
		if (nla_put_string(skb, SMC_NLA_SYS_SEID, smc_seid))
			goto errattr;
	}
	nla_nest_end(skb, attrs);
	genlmsg_end(skb, nlh);
	cb_ctx->pos[0] = 1;
	return skb->len;

errattr:
	nla_nest_cancel(skb, attrs);
errout:
	genlmsg_cancel(skb, nlh);
errmsg:
	return skb->len;
}

/* Fill SMC_NLA_LGR_D_V2_COMMON/SMC_NLA_LGR_R_V2_COMMON nested attributes */
static int smc_nl_fill_lgr_v2_common(struct smc_link_group *lgr,
				     struct sk_buff *skb,
				     struct netlink_callback *cb,
				     struct nlattr *v2_attrs)
{
	char smc_host[SMC_MAX_HOSTNAME_LEN + 1];
	char smc_eid[SMC_MAX_EID_LEN + 1];

	if (nla_put_u8(skb, SMC_NLA_LGR_V2_VER, lgr->smc_version))
		goto errv2attr;
	if (nla_put_u8(skb, SMC_NLA_LGR_V2_REL, lgr->peer_smc_release))
		goto errv2attr;
	if (nla_put_u8(skb, SMC_NLA_LGR_V2_OS, lgr->peer_os))
		goto errv2attr;
	memcpy(smc_host, lgr->peer_hostname, SMC_MAX_HOSTNAME_LEN);
	smc_host[SMC_MAX_HOSTNAME_LEN] = 0;
	if (nla_put_string(skb, SMC_NLA_LGR_V2_PEER_HOST, smc_host))
		goto errv2attr;
	memcpy(smc_eid, lgr->negotiated_eid, SMC_MAX_EID_LEN);
	smc_eid[SMC_MAX_EID_LEN] = 0;
	if (nla_put_string(skb, SMC_NLA_LGR_V2_NEG_EID, smc_eid))
		goto errv2attr;

	nla_nest_end(skb, v2_attrs);
	return 0;

errv2attr:
	nla_nest_cancel(skb, v2_attrs);
	return -EMSGSIZE;
}

static int smc_nl_fill_smcr_lgr_v2(struct smc_link_group *lgr,
				   struct sk_buff *skb,
				   struct netlink_callback *cb)
{
	struct nlattr *v2_attrs;

	v2_attrs = nla_nest_start(skb, SMC_NLA_LGR_R_V2);
	if (!v2_attrs)
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_V2_DIRECT, !lgr->uses_gateway))
		goto errv2attr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_V2_MAX_CONNS, lgr->max_conns))
		goto errv2attr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_V2_MAX_LINKS, lgr->max_links))
		goto errv2attr;

	nla_nest_end(skb, v2_attrs);
	return 0;

errv2attr:
	nla_nest_cancel(skb, v2_attrs);
errattr:
	return -EMSGSIZE;
}

static int smc_nl_fill_lgr(struct smc_link_group *lgr,
			   struct sk_buff *skb,
			   struct netlink_callback *cb)
{
	char smc_target[SMC_MAX_PNETID_LEN + 1];
	struct nlattr *attrs, *v2_attrs;

	attrs = nla_nest_start(skb, SMC_GEN_LGR_SMCR);
	if (!attrs)
		goto errout;

	if (nla_put_u32(skb, SMC_NLA_LGR_R_ID, *((u32 *)&lgr->id)))
		goto errattr;
	if (nla_put_u32(skb, SMC_NLA_LGR_R_CONNS_NUM, lgr->conns_num))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_ROLE, lgr->role))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_TYPE, lgr->type))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_BUF_TYPE, lgr->buf_type))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LGR_R_VLAN_ID, lgr->vlan_id))
		goto errattr;
	if (nla_put_u64_64bit(skb, SMC_NLA_LGR_R_NET_COOKIE,
			      lgr->net->net_cookie, SMC_NLA_LGR_R_PAD))
		goto errattr;
	memcpy(smc_target, lgr->pnet_id, SMC_MAX_PNETID_LEN);
	smc_target[SMC_MAX_PNETID_LEN] = 0;
	if (nla_put_string(skb, SMC_NLA_LGR_R_PNETID, smc_target))
		goto errattr;
	if (nla_put_uint(skb, SMC_NLA_LGR_R_SNDBUF_ALLOC, lgr->alloc_sndbufs))
		goto errattr;
	if (nla_put_uint(skb, SMC_NLA_LGR_R_RMB_ALLOC, lgr->alloc_rmbs))
		goto errattr;
	if (lgr->smc_version > SMC_V1) {
		v2_attrs = nla_nest_start(skb, SMC_NLA_LGR_R_V2_COMMON);
		if (!v2_attrs)
			goto errattr;
		if (smc_nl_fill_lgr_v2_common(lgr, skb, cb, v2_attrs))
			goto errattr;
		if (smc_nl_fill_smcr_lgr_v2(lgr, skb, cb))
			goto errattr;
	}

	nla_nest_end(skb, attrs);
	return 0;
errattr:
	nla_nest_cancel(skb, attrs);
errout:
	return -EMSGSIZE;
}

static int smc_nl_fill_lgr_link(struct smc_link_group *lgr,
				struct smc_link *link,
				struct sk_buff *skb,
				struct netlink_callback *cb)
{
	char smc_ibname[IB_DEVICE_NAME_MAX];
	u8 smc_gid_target[41];
	struct nlattr *attrs;
	u32 link_uid = 0;
	void *nlh;

	nlh = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &smc_gen_nl_family, NLM_F_MULTI,
			  SMC_NETLINK_GET_LINK_SMCR);
	if (!nlh)
		goto errmsg;

	attrs = nla_nest_start(skb, SMC_GEN_LINK_SMCR);
	if (!attrs)
		goto errout;

	if (nla_put_u8(skb, SMC_NLA_LINK_ID, link->link_id))
		goto errattr;
	if (nla_put_u32(skb, SMC_NLA_LINK_STATE, link->state))
		goto errattr;
	if (nla_put_u32(skb, SMC_NLA_LINK_CONN_CNT,
			atomic_read(&link->conn_cnt)))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LINK_IB_PORT, link->ibport))
		goto errattr;
	if (nla_put_u32(skb, SMC_NLA_LINK_NET_DEV, link->ndev_ifidx))
		goto errattr;
	snprintf(smc_ibname, sizeof(smc_ibname), "%s", link->ibname);
	if (nla_put_string(skb, SMC_NLA_LINK_IB_DEV, smc_ibname))
		goto errattr;
	memcpy(&link_uid, link->link_uid, sizeof(link_uid));
	if (nla_put_u32(skb, SMC_NLA_LINK_UID, link_uid))
		goto errattr;
	memcpy(&link_uid, link->peer_link_uid, sizeof(link_uid));
	if (nla_put_u32(skb, SMC_NLA_LINK_PEER_UID, link_uid))
		goto errattr;
	memset(smc_gid_target, 0, sizeof(smc_gid_target));
	smc_gid_be16_convert(smc_gid_target, link->gid);
	if (nla_put_string(skb, SMC_NLA_LINK_GID, smc_gid_target))
		goto errattr;
	memset(smc_gid_target, 0, sizeof(smc_gid_target));
	smc_gid_be16_convert(smc_gid_target, link->peer_gid);
	if (nla_put_string(skb, SMC_NLA_LINK_PEER_GID, smc_gid_target))
		goto errattr;

	nla_nest_end(skb, attrs);
	genlmsg_end(skb, nlh);
	return 0;
errattr:
	nla_nest_cancel(skb, attrs);
errout:
	genlmsg_cancel(skb, nlh);
errmsg:
	return -EMSGSIZE;
}

static int smc_nl_handle_lgr(struct smc_link_group *lgr,
			     struct sk_buff *skb,
			     struct netlink_callback *cb,
			     bool list_links)
{
	void *nlh;
	int i;

	nlh = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &smc_gen_nl_family, NLM_F_MULTI,
			  SMC_NETLINK_GET_LGR_SMCR);
	if (!nlh)
		goto errmsg;
	if (smc_nl_fill_lgr(lgr, skb, cb))
		goto errout;

	genlmsg_end(skb, nlh);
	if (!list_links)
		goto out;
	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		if (!smc_link_usable(&lgr->lnk[i]))
			continue;
		if (smc_nl_fill_lgr_link(lgr, &lgr->lnk[i], skb, cb))
			goto errout;
	}
out:
	return 0;

errout:
	genlmsg_cancel(skb, nlh);
errmsg:
	return -EMSGSIZE;
}

static void smc_nl_fill_lgr_list(struct smc_lgr_list *smc_lgr,
				 struct sk_buff *skb,
				 struct netlink_callback *cb,
				 bool list_links)
{
	struct smc_nl_dmp_ctx *cb_ctx = smc_nl_dmp_ctx(cb);
	struct smc_link_group *lgr;
	int snum = cb_ctx->pos[0];
	int num = 0;

	spin_lock_bh(&smc_lgr->lock);
	list_for_each_entry(lgr, &smc_lgr->list, list) {
		if (num < snum)
			goto next;
		if (smc_nl_handle_lgr(lgr, skb, cb, list_links))
			goto errout;
next:
		num++;
	}
errout:
	spin_unlock_bh(&smc_lgr->lock);
	cb_ctx->pos[0] = num;
}

static int smc_nl_fill_smcd_lgr(struct smc_link_group *lgr,
				struct sk_buff *skb,
				struct netlink_callback *cb)
{
	char smc_pnet[SMC_MAX_PNETID_LEN + 1];
	struct smcd_dev *smcd = lgr->smcd;
	struct smcd_gid smcd_gid;
	struct nlattr *attrs;
	void *nlh;

	nlh = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &smc_gen_nl_family, NLM_F_MULTI,
			  SMC_NETLINK_GET_LGR_SMCD);
	if (!nlh)
		goto errmsg;

	attrs = nla_nest_start(skb, SMC_GEN_LGR_SMCD);
	if (!attrs)
		goto errout;

	if (nla_put_u32(skb, SMC_NLA_LGR_D_ID, *((u32 *)&lgr->id)))
		goto errattr;
	smcd->ops->get_local_gid(smcd, &smcd_gid);
	if (nla_put_u64_64bit(skb, SMC_NLA_LGR_D_GID,
			      smcd_gid.gid, SMC_NLA_LGR_D_PAD))
		goto errattr;
	if (nla_put_u64_64bit(skb, SMC_NLA_LGR_D_EXT_GID,
			      smcd_gid.gid_ext, SMC_NLA_LGR_D_PAD))
		goto errattr;
	if (nla_put_u64_64bit(skb, SMC_NLA_LGR_D_PEER_GID, lgr->peer_gid.gid,
			      SMC_NLA_LGR_D_PAD))
		goto errattr;
	if (nla_put_u64_64bit(skb, SMC_NLA_LGR_D_PEER_EXT_GID,
			      lgr->peer_gid.gid_ext, SMC_NLA_LGR_D_PAD))
		goto errattr;
	if (nla_put_u8(skb, SMC_NLA_LGR_D_VLAN_ID, lgr->vlan_id))
		goto errattr;
	if (nla_put_u32(skb, SMC_NLA_LGR_D_CONNS_NUM, lgr->conns_num))
		goto errattr;
	if (nla_put_u32(skb, SMC_NLA_LGR_D_CHID, smc_ism_get_chid(lgr->smcd)))
		goto errattr;
	if (nla_put_uint(skb, SMC_NLA_LGR_D_SNDBUF_ALLOC, lgr->alloc_sndbufs))
		goto errattr;
	if (nla_put_uint(skb, SMC_NLA_LGR_D_DMB_ALLOC, lgr->alloc_rmbs))
		goto errattr;
	memcpy(smc_pnet, lgr->smcd->pnetid, SMC_MAX_PNETID_LEN);
	smc_pnet[SMC_MAX_PNETID_LEN] = 0;
	if (nla_put_string(skb, SMC_NLA_LGR_D_PNETID, smc_pnet))
		goto errattr;
	if (lgr->smc_version > SMC_V1) {
		struct nlattr *v2_attrs;

		v2_attrs = nla_nest_start(skb, SMC_NLA_LGR_D_V2_COMMON);
		if (!v2_attrs)
			goto errattr;
		if (smc_nl_fill_lgr_v2_common(lgr, skb, cb, v2_attrs))
			goto errattr;
	}
	nla_nest_end(skb, attrs);
	genlmsg_end(skb, nlh);
	return 0;

errattr:
	nla_nest_cancel(skb, attrs);
errout:
	genlmsg_cancel(skb, nlh);
errmsg:
	return -EMSGSIZE;
}

static int smc_nl_handle_smcd_lgr(struct smcd_dev *dev,
				  struct sk_buff *skb,
				  struct netlink_callback *cb)
{
	struct smc_nl_dmp_ctx *cb_ctx = smc_nl_dmp_ctx(cb);
	struct smc_link_group *lgr;
	int snum = cb_ctx->pos[1];
	int rc = 0, num = 0;

	spin_lock_bh(&dev->lgr_lock);
	list_for_each_entry(lgr, &dev->lgr_list, list) {
		if (!lgr->is_smcd)
			continue;
		if (num < snum)
			goto next;
		rc = smc_nl_fill_smcd_lgr(lgr, skb, cb);
		if (rc)
			goto errout;
next:
		num++;
	}
errout:
	spin_unlock_bh(&dev->lgr_lock);
	cb_ctx->pos[1] = num;
	return rc;
}

static int smc_nl_fill_smcd_dev(struct smcd_dev_list *dev_list,
				struct sk_buff *skb,
				struct netlink_callback *cb)
{
	struct smc_nl_dmp_ctx *cb_ctx = smc_nl_dmp_ctx(cb);
	struct smcd_dev *smcd_dev;
	int snum = cb_ctx->pos[0];
	int rc = 0, num = 0;

	mutex_lock(&dev_list->mutex);
	list_for_each_entry(smcd_dev, &dev_list->list, list) {
		if (list_empty(&smcd_dev->lgr_list))
			continue;
		if (num < snum)
			goto next;
		rc = smc_nl_handle_smcd_lgr(smcd_dev, skb, cb);
		if (rc)
			goto errout;
next:
		num++;
	}
errout:
	mutex_unlock(&dev_list->mutex);
	cb_ctx->pos[0] = num;
	return rc;
}

int smcr_nl_get_lgr(struct sk_buff *skb, struct netlink_callback *cb)
{
	bool list_links = false;

	smc_nl_fill_lgr_list(&smc_lgr_list, skb, cb, list_links);
	return skb->len;
}

int smcr_nl_get_link(struct sk_buff *skb, struct netlink_callback *cb)
{
	bool list_links = true;

	smc_nl_fill_lgr_list(&smc_lgr_list, skb, cb, list_links);
	return skb->len;
}

int smcd_nl_get_lgr(struct sk_buff *skb, struct netlink_callback *cb)
{
	smc_nl_fill_smcd_dev(&smcd_dev_list, skb, cb);
	return skb->len;
}

void smc_lgr_cleanup_early(struct smc_link_group *lgr)
{
	spinlock_t *lgr_lock;

	if (!lgr)
		return;

	smc_lgr_list_head(lgr, &lgr_lock);
	spin_lock_bh(lgr_lock);
	/* do not use this link group for new connections */
	if (!list_empty(&lgr->list))
		list_del_init(&lgr->list);
	spin_unlock_bh(lgr_lock);
	__smc_lgr_terminate(lgr, true);
}

static void smcr_lgr_link_deactivate_all(struct smc_link_group *lgr)
{
	int i;

	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		struct smc_link *lnk = &lgr->lnk[i];

		if (smc_link_sendable(lnk))
			lnk->state = SMC_LNK_INACTIVE;
	}
	wake_up_all(&lgr->llc_msg_waiter);
	wake_up_all(&lgr->llc_flow_waiter);
}

static void smc_lgr_free(struct smc_link_group *lgr);

static void smc_lgr_free_work(struct work_struct *work)
{
	struct smc_link_group *lgr = container_of(to_delayed_work(work),
						  struct smc_link_group,
						  free_work);
	spinlock_t *lgr_lock;
	bool conns;

	smc_lgr_list_head(lgr, &lgr_lock);
	spin_lock_bh(lgr_lock);
	if (lgr->freeing) {
		spin_unlock_bh(lgr_lock);
		return;
	}
	read_lock_bh(&lgr->conns_lock);
	conns = RB_EMPTY_ROOT(&lgr->conns_all);
	read_unlock_bh(&lgr->conns_lock);
	if (!conns) { /* number of lgr connections is no longer zero */
		spin_unlock_bh(lgr_lock);
		return;
	}
	list_del_init(&lgr->list); /* remove from smc_lgr_list */
	lgr->freeing = 1; /* this instance does the freeing, no new schedule */
	spin_unlock_bh(lgr_lock);
	cancel_delayed_work(&lgr->free_work);

	if (!lgr->is_smcd && !lgr->terminating)
		smc_llc_send_link_delete_all(lgr, true,
					     SMC_LLC_DEL_PROG_INIT_TERM);
	if (lgr->is_smcd && !lgr->terminating)
		smc_ism_signal_shutdown(lgr);
	if (!lgr->is_smcd)
		smcr_lgr_link_deactivate_all(lgr);
	smc_lgr_free(lgr);
}

static void smc_lgr_terminate_work(struct work_struct *work)
{
	struct smc_link_group *lgr = container_of(work, struct smc_link_group,
						  terminate_work);

	__smc_lgr_terminate(lgr, true);
}

/* return next unique link id for the lgr */
static u8 smcr_next_link_id(struct smc_link_group *lgr)
{
	u8 link_id;
	int i;

	while (1) {
again:
		link_id = ++lgr->next_link_id;
		if (!link_id)	/* skip zero as link_id */
			link_id = ++lgr->next_link_id;
		for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
			if (smc_link_usable(&lgr->lnk[i]) &&
			    lgr->lnk[i].link_id == link_id)
				goto again;
		}
		break;
	}
	return link_id;
}

static void smcr_copy_dev_info_to_link(struct smc_link *link)
{
	struct smc_ib_device *smcibdev = link->smcibdev;

	snprintf(link->ibname, sizeof(link->ibname), "%s",
		 smcibdev->ibdev->name);
	link->ndev_ifidx = smcibdev->ndev_ifidx[link->ibport - 1];
}

int smcr_link_init(struct smc_link_group *lgr, struct smc_link *lnk,
		   u8 link_idx, struct smc_init_info *ini)
{
	struct smc_ib_device *smcibdev;
	u8 rndvec[3];
	int rc;

	if (lgr->smc_version == SMC_V2) {
		lnk->smcibdev = ini->smcrv2.ib_dev_v2;
		lnk->ibport = ini->smcrv2.ib_port_v2;
		lnk->wr_rx_sge_cnt = lnk->smcibdev->ibdev->attrs.max_recv_sge < 2 ? 1 : 2;
		lnk->wr_rx_buflen = smc_link_shared_v2_rxbuf(lnk) ?
			SMC_WR_BUF_SIZE : SMC_WR_BUF_V2_SIZE;
	} else {
		lnk->smcibdev = ini->ib_dev;
		lnk->ibport = ini->ib_port;
		lnk->wr_rx_sge_cnt = 1;
		lnk->wr_rx_buflen = SMC_WR_BUF_SIZE;
	}
	get_device(&lnk->smcibdev->ibdev->dev);
	atomic_inc(&lnk->smcibdev->lnk_cnt);
	refcount_set(&lnk->refcnt, 1); /* link refcnt is set to 1 */
	lnk->clearing = 0;
	lnk->path_mtu = lnk->smcibdev->pattr[lnk->ibport - 1].active_mtu;
	lnk->link_id = smcr_next_link_id(lgr);
	lnk->lgr = lgr;
	smc_lgr_hold(lgr); /* lgr_put in smcr_link_clear() */
	lnk->link_idx = link_idx;
	lnk->wr_rx_id_compl = 0;
	smc_ibdev_cnt_inc(lnk);
	smcr_copy_dev_info_to_link(lnk);
	atomic_set(&lnk->conn_cnt, 0);
	smc_llc_link_set_uid(lnk);
	INIT_WORK(&lnk->link_down_wrk, smc_link_down_work);
	if (!lnk->smcibdev->initialized) {
		rc = (int)smc_ib_setup_per_ibdev(lnk->smcibdev);
		if (rc)
			goto out;
	}
	get_random_bytes(rndvec, sizeof(rndvec));
	lnk->psn_initial = rndvec[0] + (rndvec[1] << 8) +
		(rndvec[2] << 16);
	rc = smc_ib_determine_gid(lnk->smcibdev, lnk->ibport,
				  ini->vlan_id, lnk->gid, &lnk->sgid_index,
				  lgr->smc_version == SMC_V2 ?
						  &ini->smcrv2 : NULL);
	if (rc)
		goto out;
	rc = smc_llc_link_init(lnk);
	if (rc)
		goto out;
	rc = smc_wr_alloc_link_mem(lnk);
	if (rc)
		goto clear_llc_lnk;
	rc = smc_ib_create_protection_domain(lnk);
	if (rc)
		goto free_link_mem;
	rc = smc_ib_create_queue_pair(lnk);
	if (rc)
		goto dealloc_pd;
	rc = smc_wr_create_link(lnk);
	if (rc)
		goto destroy_qp;
	lnk->state = SMC_LNK_ACTIVATING;
	return 0;

destroy_qp:
	smc_ib_destroy_queue_pair(lnk);
dealloc_pd:
	smc_ib_dealloc_protection_domain(lnk);
free_link_mem:
	smc_wr_free_link_mem(lnk);
clear_llc_lnk:
	smc_llc_link_clear(lnk, false);
out:
	smc_ibdev_cnt_dec(lnk);
	put_device(&lnk->smcibdev->ibdev->dev);
	smcibdev = lnk->smcibdev;
	memset(lnk, 0, sizeof(struct smc_link));
	lnk->state = SMC_LNK_UNUSED;
	if (!atomic_dec_return(&smcibdev->lnk_cnt))
		wake_up(&smcibdev->lnks_deleted);
	smc_lgr_put(lgr); /* lgr_hold above */
	return rc;
}

/* create a new SMC link group */
static int smc_lgr_create(struct smc_sock *smc, struct smc_init_info *ini)
{
	struct smc_link_group *lgr;
	struct list_head *lgr_list;
	struct smcd_dev *smcd;
	struct smc_link *lnk;
	spinlock_t *lgr_lock;
	u8 link_idx;
	int rc = 0;
	int i;

	if (ini->is_smcd && ini->vlan_id) {
		if (smc_ism_get_vlan(ini->ism_dev[ini->ism_selected],
				     ini->vlan_id)) {
			rc = SMC_CLC_DECL_ISMVLANERR;
			goto out;
		}
	}

	lgr = kzalloc(sizeof(*lgr), GFP_KERNEL);
	if (!lgr) {
		rc = SMC_CLC_DECL_MEM;
		goto ism_put_vlan;
	}
	lgr->tx_wq = alloc_workqueue("smc_tx_wq-%*phN", 0, 0,
				     SMC_LGR_ID_SIZE, &lgr->id);
	if (!lgr->tx_wq) {
		rc = -ENOMEM;
		goto free_lgr;
	}
	lgr->is_smcd = ini->is_smcd;
	lgr->sync_err = 0;
	lgr->terminating = 0;
	lgr->freeing = 0;
	lgr->vlan_id = ini->vlan_id;
	refcount_set(&lgr->refcnt, 1); /* set lgr refcnt to 1 */
	init_rwsem(&lgr->sndbufs_lock);
	init_rwsem(&lgr->rmbs_lock);
	rwlock_init(&lgr->conns_lock);
	for (i = 0; i < SMC_RMBE_SIZES; i++) {
		INIT_LIST_HEAD(&lgr->sndbufs[i]);
		INIT_LIST_HEAD(&lgr->rmbs[i]);
	}
	lgr->next_link_id = 0;
	smc_lgr_list.num += SMC_LGR_NUM_INCR;
	memcpy(&lgr->id, (u8 *)&smc_lgr_list.num, SMC_LGR_ID_SIZE);
	INIT_DELAYED_WORK(&lgr->free_work, smc_lgr_free_work);
	INIT_WORK(&lgr->terminate_work, smc_lgr_terminate_work);
	lgr->conns_all = RB_ROOT;
	if (ini->is_smcd) {
		/* SMC-D specific settings */
		smcd = ini->ism_dev[ini->ism_selected];
		get_device(smcd->ops->get_dev(smcd));
		lgr->peer_gid.gid =
			ini->ism_peer_gid[ini->ism_selected].gid;
		lgr->peer_gid.gid_ext =
			ini->ism_peer_gid[ini->ism_selected].gid_ext;
		lgr->smcd = ini->ism_dev[ini->ism_selected];
		lgr_list = &ini->ism_dev[ini->ism_selected]->lgr_list;
		lgr_lock = &lgr->smcd->lgr_lock;
		lgr->smc_version = ini->smcd_version;
		lgr->peer_shutdown = 0;
		atomic_inc(&ini->ism_dev[ini->ism_selected]->lgr_cnt);
	} else {
		/* SMC-R specific settings */
		struct smc_ib_device *ibdev;
		int ibport;

		lgr->role = smc->listen_smc ? SMC_SERV : SMC_CLNT;
		lgr->smc_version = ini->smcr_version;
		memcpy(lgr->peer_systemid, ini->peer_systemid,
		       SMC_SYSTEMID_LEN);
		if (lgr->smc_version == SMC_V2) {
			ibdev = ini->smcrv2.ib_dev_v2;
			ibport = ini->smcrv2.ib_port_v2;
			lgr->saddr = ini->smcrv2.saddr;
			lgr->uses_gateway = ini->smcrv2.uses_gateway;
			memcpy(lgr->nexthop_mac, ini->smcrv2.nexthop_mac,
			       ETH_ALEN);
			lgr->max_conns = ini->max_conns;
			lgr->max_links = ini->max_links;
		} else {
			ibdev = ini->ib_dev;
			ibport = ini->ib_port;
			lgr->max_conns = SMC_CONN_PER_LGR_MAX;
			lgr->max_links = SMC_LINKS_ADD_LNK_MAX;
		}
		memcpy(lgr->pnet_id, ibdev->pnetid[ibport - 1],
		       SMC_MAX_PNETID_LEN);
		rc = smc_wr_alloc_lgr_mem(lgr);
		if (rc)
			goto free_wq;
		smc_llc_lgr_init(lgr, smc);

		link_idx = SMC_SINGLE_LINK;
		lnk = &lgr->lnk[link_idx];
		rc = smcr_link_init(lgr, lnk, link_idx, ini);
		if (rc) {
			smc_wr_free_lgr_mem(lgr);
			goto free_wq;
		}
		lgr->net = smc_ib_net(lnk->smcibdev);
		lgr_list = &smc_lgr_list.list;
		lgr_lock = &smc_lgr_list.lock;
		lgr->buf_type = lgr->net->smc.sysctl_smcr_buf_type;
		atomic_inc(&lgr_cnt);
	}
	smc->conn.lgr = lgr;
	spin_lock_bh(lgr_lock);
	list_add_tail(&lgr->list, lgr_list);
	spin_unlock_bh(lgr_lock);
	return 0;

free_wq:
	destroy_workqueue(lgr->tx_wq);
free_lgr:
	kfree(lgr);
ism_put_vlan:
	if (ini->is_smcd && ini->vlan_id)
		smc_ism_put_vlan(ini->ism_dev[ini->ism_selected], ini->vlan_id);
out:
	if (rc < 0) {
		if (rc == -ENOMEM)
			rc = SMC_CLC_DECL_MEM;
		else
			rc = SMC_CLC_DECL_INTERR;
	}
	return rc;
}

static int smc_write_space(struct smc_connection *conn)
{
	int buffer_len = conn->peer_rmbe_size;
	union smc_host_cursor prod;
	union smc_host_cursor cons;
	int space;

	smc_curs_copy(&prod, &conn->local_tx_ctrl.prod, conn);
	smc_curs_copy(&cons, &conn->local_rx_ctrl.cons, conn);
	/* determine rx_buf space */
	space = buffer_len - smc_curs_diff(buffer_len, &cons, &prod);
	return space;
}

static int smc_switch_cursor(struct smc_sock *smc, struct smc_cdc_tx_pend *pend,
			     struct smc_wr_buf *wr_buf)
{
	struct smc_connection *conn = &smc->conn;
	union smc_host_cursor cons, fin;
	int rc = 0;
	int diff;

	smc_curs_copy(&conn->tx_curs_sent, &conn->tx_curs_fin, conn);
	smc_curs_copy(&fin, &conn->local_tx_ctrl_fin, conn);
	/* set prod cursor to old state, enforce tx_rdma_writes() */
	smc_curs_copy(&conn->local_tx_ctrl.prod, &fin, conn);
	smc_curs_copy(&cons, &conn->local_rx_ctrl.cons, conn);

	if (smc_curs_comp(conn->peer_rmbe_size, &cons, &fin) < 0) {
		/* cons cursor advanced more than fin, and prod was set
		 * fin above, so now prod is smaller than cons. Fix that.
		 */
		diff = smc_curs_diff(conn->peer_rmbe_size, &fin, &cons);
		smc_curs_add(conn->sndbuf_desc->len,
			     &conn->tx_curs_sent, diff);
		smc_curs_add(conn->sndbuf_desc->len,
			     &conn->tx_curs_fin, diff);

		smp_mb__before_atomic();
		atomic_add(diff, &conn->sndbuf_space);
		smp_mb__after_atomic();

		smc_curs_add(conn->peer_rmbe_size,
			     &conn->local_tx_ctrl.prod, diff);
		smc_curs_add(conn->peer_rmbe_size,
			     &conn->local_tx_ctrl_fin, diff);
	}
	/* recalculate, value is used by tx_rdma_writes() */
	atomic_set(&smc->conn.peer_rmbe_space, smc_write_space(conn));

	if (smc->sk.sk_state != SMC_INIT &&
	    smc->sk.sk_state != SMC_CLOSED) {
		rc = smcr_cdc_msg_send_validation(conn, pend, wr_buf);
		if (!rc) {
			queue_delayed_work(conn->lgr->tx_wq, &conn->tx_work, 0);
			smc->sk.sk_data_ready(&smc->sk);
		}
	} else {
		smc_wr_tx_put_slot(conn->lnk,
				   (struct smc_wr_tx_pend_priv *)pend);
	}
	return rc;
}

void smc_switch_link_and_count(struct smc_connection *conn,
			       struct smc_link *to_lnk)
{
	atomic_dec(&conn->lnk->conn_cnt);
	/* link_hold in smc_conn_create() */
	smcr_link_put(conn->lnk);
	conn->lnk = to_lnk;
	atomic_inc(&conn->lnk->conn_cnt);
	/* link_put in smc_conn_free() */
	smcr_link_hold(conn->lnk);
}

struct smc_link *smc_switch_conns(struct smc_link_group *lgr,
				  struct smc_link *from_lnk, bool is_dev_err)
{
	struct smc_link *to_lnk = NULL;
	struct smc_cdc_tx_pend *pend;
	struct smc_connection *conn;
	struct smc_wr_buf *wr_buf;
	struct smc_sock *smc;
	struct rb_node *node;
	int i, rc = 0;

	/* link is inactive, wake up tx waiters */
	smc_wr_wakeup_tx_wait(from_lnk);

	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		if (!smc_link_active(&lgr->lnk[i]) || i == from_lnk->link_idx)
			continue;
		if (is_dev_err && from_lnk->smcibdev == lgr->lnk[i].smcibdev &&
		    from_lnk->ibport == lgr->lnk[i].ibport) {
			continue;
		}
		to_lnk = &lgr->lnk[i];
		break;
	}
	if (!to_lnk || !smc_wr_tx_link_hold(to_lnk)) {
		smc_lgr_terminate_sched(lgr);
		return NULL;
	}
again:
	read_lock_bh(&lgr->conns_lock);
	for (node = rb_first(&lgr->conns_all); node; node = rb_next(node)) {
		conn = rb_entry(node, struct smc_connection, alert_node);
		if (conn->lnk != from_lnk)
			continue;
		smc = container_of(conn, struct smc_sock, conn);
		/* conn->lnk not yet set in SMC_INIT state */
		if (smc->sk.sk_state == SMC_INIT)
			continue;
		if (smc->sk.sk_state == SMC_CLOSED ||
		    smc->sk.sk_state == SMC_PEERCLOSEWAIT1 ||
		    smc->sk.sk_state == SMC_PEERCLOSEWAIT2 ||
		    smc->sk.sk_state == SMC_APPFINCLOSEWAIT ||
		    smc->sk.sk_state == SMC_APPCLOSEWAIT1 ||
		    smc->sk.sk_state == SMC_APPCLOSEWAIT2 ||
		    smc->sk.sk_state == SMC_PEERFINCLOSEWAIT ||
		    smc->sk.sk_state == SMC_PEERABORTWAIT ||
		    smc->sk.sk_state == SMC_PROCESSABORT) {
			spin_lock_bh(&conn->send_lock);
			smc_switch_link_and_count(conn, to_lnk);
			spin_unlock_bh(&conn->send_lock);
			continue;
		}
		sock_hold(&smc->sk);
		read_unlock_bh(&lgr->conns_lock);
		/* pre-fetch buffer outside of send_lock, might sleep */
		rc = smc_cdc_get_free_slot(conn, to_lnk, &wr_buf, NULL, &pend);
		if (rc)
			goto err_out;
		/* avoid race with smcr_tx_sndbuf_nonempty() */
		spin_lock_bh(&conn->send_lock);
		smc_switch_link_and_count(conn, to_lnk);
		rc = smc_switch_cursor(smc, pend, wr_buf);
		spin_unlock_bh(&conn->send_lock);
		sock_put(&smc->sk);
		if (rc)
			goto err_out;
		goto again;
	}
	read_unlock_bh(&lgr->conns_lock);
	smc_wr_tx_link_put(to_lnk);
	return to_lnk;

err_out:
	smcr_link_down_cond_sched(to_lnk);
	smc_wr_tx_link_put(to_lnk);
	return NULL;
}

static void smcr_buf_unuse(struct smc_buf_desc *buf_desc, bool is_rmb,
			   struct smc_link_group *lgr)
{
	struct rw_semaphore *lock;	/* lock buffer list */
	int rc;

	if (is_rmb && buf_desc->is_conf_rkey && !list_empty(&lgr->list)) {
		/* unregister rmb with peer */
		rc = smc_llc_flow_initiate(lgr, SMC_LLC_FLOW_RKEY);
		if (!rc) {
			/* protect against smc_llc_cli_rkey_exchange() */
			down_read(&lgr->llc_conf_mutex);
			smc_llc_do_delete_rkey(lgr, buf_desc);
			buf_desc->is_conf_rkey = false;
			up_read(&lgr->llc_conf_mutex);
			smc_llc_flow_stop(lgr, &lgr->llc_flow_lcl);
		}
	}

	if (buf_desc->is_reg_err) {
		/* buf registration failed, reuse not possible */
		lock = is_rmb ? &lgr->rmbs_lock :
				&lgr->sndbufs_lock;
		down_write(lock);
		smc_lgr_buf_list_del(lgr, is_rmb, buf_desc);
		up_write(lock);

		smc_buf_free(lgr, is_rmb, buf_desc);
	} else {
		/* memzero_explicit provides potential memory barrier semantics */
		memzero_explicit(buf_desc->cpu_addr, buf_desc->len);
		WRITE_ONCE(buf_desc->used, 0);
	}
}

static void smcd_buf_detach(struct smc_connection *conn)
{
	struct smcd_dev *smcd = conn->lgr->smcd;
	u64 peer_token = conn->peer_token;

	if (!conn->sndbuf_desc)
		return;

	smc_ism_detach_dmb(smcd, peer_token);

	kfree(conn->sndbuf_desc);
	conn->sndbuf_desc = NULL;
}

static void smc_buf_unuse(struct smc_connection *conn,
			  struct smc_link_group *lgr)
{
	struct smc_sock *smc = container_of(conn, struct smc_sock, conn);
	bool is_smcd = lgr->is_smcd;
	int bufsize;

	if (conn->sndbuf_desc) {
		bufsize = conn->sndbuf_desc->len;
		if (!is_smcd && conn->sndbuf_desc->is_vm) {
			smcr_buf_unuse(conn->sndbuf_desc, false, lgr);
		} else {
			memzero_explicit(conn->sndbuf_desc->cpu_addr, bufsize);
			WRITE_ONCE(conn->sndbuf_desc->used, 0);
		}
		SMC_STAT_RMB_SIZE(smc, is_smcd, false, false, bufsize);
	}
	if (conn->rmb_desc) {
		bufsize = conn->rmb_desc->len;
		if (!is_smcd) {
			smcr_buf_unuse(conn->rmb_desc, true, lgr);
		} else {
			bufsize += sizeof(struct smcd_cdc_msg);
			memzero_explicit(conn->rmb_desc->cpu_addr, bufsize);
			WRITE_ONCE(conn->rmb_desc->used, 0);
		}
		SMC_STAT_RMB_SIZE(smc, is_smcd, true, false, bufsize);
	}
}

/* remove a finished connection from its link group */
void smc_conn_free(struct smc_connection *conn)
{
	struct smc_link_group *lgr = conn->lgr;

	if (!lgr || conn->freed)
		/* Connection has never been registered in a
		 * link group, or has already been freed.
		 */
		return;

	conn->freed = 1;
	if (!smc_conn_lgr_valid(conn))
		/* Connection has already unregistered from
		 * link group.
		 */
		goto lgr_put;

	if (lgr->is_smcd) {
		if (!list_empty(&lgr->list))
			smc_ism_unset_conn(conn);
		if (smc_ism_support_dmb_nocopy(lgr->smcd))
			smcd_buf_detach(conn);
		tasklet_kill(&conn->rx_tsklet);
	} else {
		smc_cdc_wait_pend_tx_wr(conn);
		if (current_work() != &conn->abort_work)
			cancel_work_sync(&conn->abort_work);
	}
	if (!list_empty(&lgr->list)) {
		smc_buf_unuse(conn, lgr); /* allow buffer reuse */
		smc_lgr_unregister_conn(conn);
	}

	if (!lgr->conns_num)
		smc_lgr_schedule_free_work(lgr);
lgr_put:
	if (!lgr->is_smcd)
		smcr_link_put(conn->lnk); /* link_hold in smc_conn_create() */
	smc_lgr_put(lgr); /* lgr_hold in smc_conn_create() */
}

/* unregister a link from a buf_desc */
static void smcr_buf_unmap_link(struct smc_buf_desc *buf_desc, bool is_rmb,
				struct smc_link *lnk)
{
	if (is_rmb || buf_desc->is_vm)
		buf_desc->is_reg_mr[lnk->link_idx] = false;
	if (!buf_desc->is_map_ib[lnk->link_idx])
		return;

	if ((is_rmb || buf_desc->is_vm) &&
	    buf_desc->mr[lnk->link_idx]) {
		smc_ib_put_memory_region(buf_desc->mr[lnk->link_idx]);
		buf_desc->mr[lnk->link_idx] = NULL;
	}
	if (is_rmb)
		smc_ib_buf_unmap_sg(lnk, buf_desc, DMA_FROM_DEVICE);
	else
		smc_ib_buf_unmap_sg(lnk, buf_desc, DMA_TO_DEVICE);

	sg_free_table(&buf_desc->sgt[lnk->link_idx]);
	buf_desc->is_map_ib[lnk->link_idx] = false;
}

/* unmap all buffers of lgr for a deleted link */
static void smcr_buf_unmap_lgr(struct smc_link *lnk)
{
	struct smc_link_group *lgr = lnk->lgr;
	struct smc_buf_desc *buf_desc, *bf;
	int i;

	for (i = 0; i < SMC_RMBE_SIZES; i++) {
		down_write(&lgr->rmbs_lock);
		list_for_each_entry_safe(buf_desc, bf, &lgr->rmbs[i], list)
			smcr_buf_unmap_link(buf_desc, true, lnk);
		up_write(&lgr->rmbs_lock);

		down_write(&lgr->sndbufs_lock);
		list_for_each_entry_safe(buf_desc, bf, &lgr->sndbufs[i],
					 list)
			smcr_buf_unmap_link(buf_desc, false, lnk);
		up_write(&lgr->sndbufs_lock);
	}
}

static void smcr_rtoken_clear_link(struct smc_link *lnk)
{
	struct smc_link_group *lgr = lnk->lgr;
	int i;

	for (i = 0; i < SMC_RMBS_PER_LGR_MAX; i++) {
		lgr->rtokens[i][lnk->link_idx].rkey = 0;
		lgr->rtokens[i][lnk->link_idx].dma_addr = 0;
	}
}

static void __smcr_link_clear(struct smc_link *lnk)
{
	struct smc_link_group *lgr = lnk->lgr;
	struct smc_ib_device *smcibdev;

	smc_wr_free_link_mem(lnk);
	smc_ibdev_cnt_dec(lnk);
	put_device(&lnk->smcibdev->ibdev->dev);
	smcibdev = lnk->smcibdev;
	memset(lnk, 0, sizeof(struct smc_link));
	lnk->state = SMC_LNK_UNUSED;
	if (!atomic_dec_return(&smcibdev->lnk_cnt))
		wake_up(&smcibdev->lnks_deleted);
	smc_lgr_put(lgr); /* lgr_hold in smcr_link_init() */
}

/* must be called under lgr->llc_conf_mutex lock */
void smcr_link_clear(struct smc_link *lnk, bool log)
{
	if (!lnk->lgr || lnk->clearing ||
	    lnk->state == SMC_LNK_UNUSED)
		return;
	lnk->clearing = 1;
	lnk->peer_qpn = 0;
	smc_llc_link_clear(lnk, log);
	smcr_buf_unmap_lgr(lnk);
	smcr_rtoken_clear_link(lnk);
	smc_ib_modify_qp_error(lnk);
	smc_wr_free_link(lnk);
	smc_ib_destroy_queue_pair(lnk);
	smc_ib_dealloc_protection_domain(lnk);
	smcr_link_put(lnk); /* theoretically last link_put */
}

void smcr_link_hold(struct smc_link *lnk)
{
	refcount_inc(&lnk->refcnt);
}

void smcr_link_put(struct smc_link *lnk)
{
	if (refcount_dec_and_test(&lnk->refcnt))
		__smcr_link_clear(lnk);
}

static void smcr_buf_free(struct smc_link_group *lgr, bool is_rmb,
			  struct smc_buf_desc *buf_desc)
{
	int i;

	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++)
		smcr_buf_unmap_link(buf_desc, is_rmb, &lgr->lnk[i]);

	if (!buf_desc->is_vm && buf_desc->pages)
		__free_pages(buf_desc->pages, buf_desc->order);
	else if (buf_desc->is_vm && buf_desc->cpu_addr)
		vfree(buf_desc->cpu_addr);
	kfree(buf_desc);
}

static void smcd_buf_free(struct smc_link_group *lgr, bool is_dmb,
			  struct smc_buf_desc *buf_desc)
{
	if (is_dmb) {
		/* restore original buf len */
		buf_desc->len += sizeof(struct smcd_cdc_msg);
		smc_ism_unregister_dmb(lgr->smcd, buf_desc);
	} else {
		kfree(buf_desc->cpu_addr);
	}
	kfree(buf_desc);
}

static void smc_buf_free(struct smc_link_group *lgr, bool is_rmb,
			 struct smc_buf_desc *buf_desc)
{
	if (lgr->is_smcd)
		smcd_buf_free(lgr, is_rmb, buf_desc);
	else
		smcr_buf_free(lgr, is_rmb, buf_desc);
}

static void __smc_lgr_free_bufs(struct smc_link_group *lgr, bool is_rmb)
{
	struct smc_buf_desc *buf_desc, *bf_desc;
	struct list_head *buf_list;
	int i;

	for (i = 0; i < SMC_RMBE_SIZES; i++) {
		if (is_rmb)
			buf_list = &lgr->rmbs[i];
		else
			buf_list = &lgr->sndbufs[i];
		list_for_each_entry_safe(buf_desc, bf_desc, buf_list,
					 list) {
			smc_lgr_buf_list_del(lgr, is_rmb, buf_desc);
			smc_buf_free(lgr, is_rmb, buf_desc);
		}
	}
}

static void smc_lgr_free_bufs(struct smc_link_group *lgr)
{
	/* free send buffers */
	__smc_lgr_free_bufs(lgr, false);
	/* free rmbs */
	__smc_lgr_free_bufs(lgr, true);
}

/* won't be freed until no one accesses to lgr anymore */
static void __smc_lgr_free(struct smc_link_group *lgr)
{
	smc_lgr_free_bufs(lgr);
	if (lgr->is_smcd) {
		if (!atomic_dec_return(&lgr->smcd->lgr_cnt))
			wake_up(&lgr->smcd->lgrs_deleted);
	} else {
		smc_wr_free_lgr_mem(lgr);
		if (!atomic_dec_return(&lgr_cnt))
			wake_up(&lgrs_deleted);
	}
	kfree(lgr);
}

/* remove a link group */
static void smc_lgr_free(struct smc_link_group *lgr)
{
	int i;

	if (!lgr->is_smcd) {
		down_write(&lgr->llc_conf_mutex);
		for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
			if (lgr->lnk[i].state != SMC_LNK_UNUSED)
				smcr_link_clear(&lgr->lnk[i], false);
		}
		up_write(&lgr->llc_conf_mutex);
		smc_llc_lgr_clear(lgr);
	}

	destroy_workqueue(lgr->tx_wq);
	if (lgr->is_smcd) {
		smc_ism_put_vlan(lgr->smcd, lgr->vlan_id);
		put_device(lgr->smcd->ops->get_dev(lgr->smcd));
	}
	smc_lgr_put(lgr); /* theoretically last lgr_put */
}

void smc_lgr_hold(struct smc_link_group *lgr)
{
	refcount_inc(&lgr->refcnt);
}

void smc_lgr_put(struct smc_link_group *lgr)
{
	if (refcount_dec_and_test(&lgr->refcnt))
		__smc_lgr_free(lgr);
}

static void smc_sk_wake_ups(struct smc_sock *smc)
{
	smc->sk.sk_write_space(&smc->sk);
	smc->sk.sk_data_ready(&smc->sk);
	smc->sk.sk_state_change(&smc->sk);
}

/* kill a connection */
static void smc_conn_kill(struct smc_connection *conn, bool soft)
{
	struct smc_sock *smc = container_of(conn, struct smc_sock, conn);

	if (conn->lgr->is_smcd && conn->lgr->peer_shutdown)
		conn->local_tx_ctrl.conn_state_flags.peer_conn_abort = 1;
	else
		smc_close_abort(conn);
	conn->killed = 1;
	smc->sk.sk_err = ECONNABORTED;
	smc_sk_wake_ups(smc);
	if (conn->lgr->is_smcd) {
		smc_ism_unset_conn(conn);
		if (smc_ism_support_dmb_nocopy(conn->lgr->smcd))
			smcd_buf_detach(conn);
		if (soft)
			tasklet_kill(&conn->rx_tsklet);
		else
			tasklet_unlock_wait(&conn->rx_tsklet);
	} else {
		smc_cdc_wait_pend_tx_wr(conn);
	}
	smc_lgr_unregister_conn(conn);
	smc_close_active_abort(smc);
}

static void smc_lgr_cleanup(struct smc_link_group *lgr)
{
	if (lgr->is_smcd) {
		smc_ism_signal_shutdown(lgr);
	} else {
		u32 rsn = lgr->llc_termination_rsn;

		if (!rsn)
			rsn = SMC_LLC_DEL_PROG_INIT_TERM;
		smc_llc_send_link_delete_all(lgr, false, rsn);
		smcr_lgr_link_deactivate_all(lgr);
	}
}

/* terminate link group
 * @soft: true if link group shutdown can take its time
 *	  false if immediate link group shutdown is required
 */
static void __smc_lgr_terminate(struct smc_link_group *lgr, bool soft)
{
	struct smc_connection *conn;
	struct smc_sock *smc;
	struct rb_node *node;

	if (lgr->terminating)
		return;	/* lgr already terminating */
	/* cancel free_work sync, will terminate when lgr->freeing is set */
	cancel_delayed_work(&lgr->free_work);
	lgr->terminating = 1;

	/* kill remaining link group connections */
	read_lock_bh(&lgr->conns_lock);
	node = rb_first(&lgr->conns_all);
	while (node) {
		read_unlock_bh(&lgr->conns_lock);
		conn = rb_entry(node, struct smc_connection, alert_node);
		smc = container_of(conn, struct smc_sock, conn);
		sock_hold(&smc->sk); /* sock_put below */
		lock_sock(&smc->sk);
		smc_conn_kill(conn, soft);
		release_sock(&smc->sk);
		sock_put(&smc->sk); /* sock_hold above */
		read_lock_bh(&lgr->conns_lock);
		node = rb_first(&lgr->conns_all);
	}
	read_unlock_bh(&lgr->conns_lock);
	smc_lgr_cleanup(lgr);
	smc_lgr_free(lgr);
}

/* unlink link group and schedule termination */
void smc_lgr_terminate_sched(struct smc_link_group *lgr)
{
	spinlock_t *lgr_lock;

	smc_lgr_list_head(lgr, &lgr_lock);
	spin_lock_bh(lgr_lock);
	if (list_empty(&lgr->list) || lgr->terminating || lgr->freeing) {
		spin_unlock_bh(lgr_lock);
		return;	/* lgr already terminating */
	}
	list_del_init(&lgr->list);
	lgr->freeing = 1;
	spin_unlock_bh(lgr_lock);
	schedule_work(&lgr->terminate_work);
}

/* Called when peer lgr shutdown (regularly or abnormally) is received */
void smc_smcd_terminate(struct smcd_dev *dev, struct smcd_gid *peer_gid,
			unsigned short vlan)
{
	struct smc_link_group *lgr, *l;
	LIST_HEAD(lgr_free_list);

	/* run common cleanup function and build free list */
	spin_lock_bh(&dev->lgr_lock);
	list_for_each_entry_safe(lgr, l, &dev->lgr_list, list) {
		if ((!peer_gid->gid ||
		     (lgr->peer_gid.gid == peer_gid->gid &&
		      !smc_ism_is_emulated(dev) ? 1 :
		      lgr->peer_gid.gid_ext == peer_gid->gid_ext)) &&
		    (vlan == VLAN_VID_MASK || lgr->vlan_id == vlan)) {
			if (peer_gid->gid) /* peer triggered termination */
				lgr->peer_shutdown = 1;
			list_move(&lgr->list, &lgr_free_list);
			lgr->freeing = 1;
		}
	}
	spin_unlock_bh(&dev->lgr_lock);

	/* cancel the regular free workers and actually free lgrs */
	list_for_each_entry_safe(lgr, l, &lgr_free_list, list) {
		list_del_init(&lgr->list);
		schedule_work(&lgr->terminate_work);
	}
}

/* Called when an SMCD device is removed or the smc module is unloaded */
void smc_smcd_terminate_all(struct smcd_dev *smcd)
{
	struct smc_link_group *lgr, *lg;
	LIST_HEAD(lgr_free_list);

	spin_lock_bh(&smcd->lgr_lock);
	list_splice_init(&smcd->lgr_list, &lgr_free_list);
	list_for_each_entry(lgr, &lgr_free_list, list)
		lgr->freeing = 1;
	spin_unlock_bh(&smcd->lgr_lock);

	list_for_each_entry_safe(lgr, lg, &lgr_free_list, list) {
		list_del_init(&lgr->list);
		__smc_lgr_terminate(lgr, false);
	}

	if (atomic_read(&smcd->lgr_cnt))
		wait_event(smcd->lgrs_deleted, !atomic_read(&smcd->lgr_cnt));
}

/* Called when an SMCR device is removed or the smc module is unloaded.
 * If smcibdev is given, all SMCR link groups using this device are terminated.
 * If smcibdev is NULL, all SMCR link groups are terminated.
 */
void smc_smcr_terminate_all(struct smc_ib_device *smcibdev)
{
	struct smc_link_group *lgr, *lg;
	LIST_HEAD(lgr_free_list);
	int i;

	spin_lock_bh(&smc_lgr_list.lock);
	if (!smcibdev) {
		list_splice_init(&smc_lgr_list.list, &lgr_free_list);
		list_for_each_entry(lgr, &lgr_free_list, list)
			lgr->freeing = 1;
	} else {
		list_for_each_entry_safe(lgr, lg, &smc_lgr_list.list, list) {
			for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
				if (lgr->lnk[i].smcibdev == smcibdev)
					smcr_link_down_cond_sched(&lgr->lnk[i]);
			}
		}
	}
	spin_unlock_bh(&smc_lgr_list.lock);

	list_for_each_entry_safe(lgr, lg, &lgr_free_list, list) {
		list_del_init(&lgr->list);
		smc_llc_set_termination_rsn(lgr, SMC_LLC_DEL_OP_INIT_TERM);
		__smc_lgr_terminate(lgr, false);
	}

	if (smcibdev) {
		if (atomic_read(&smcibdev->lnk_cnt))
			wait_event(smcibdev->lnks_deleted,
				   !atomic_read(&smcibdev->lnk_cnt));
	} else {
		if (atomic_read(&lgr_cnt))
			wait_event(lgrs_deleted, !atomic_read(&lgr_cnt));
	}
}

/* set new lgr type and clear all asymmetric link tagging */
void smcr_lgr_set_type(struct smc_link_group *lgr, enum smc_lgr_type new_type)
{
	char *lgr_type = "";
	int i;

	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++)
		if (smc_link_usable(&lgr->lnk[i]))
			lgr->lnk[i].link_is_asym = false;
	if (lgr->type == new_type)
		return;
	lgr->type = new_type;

	switch (lgr->type) {
	case SMC_LGR_NONE:
		lgr_type = "NONE";
		break;
	case SMC_LGR_SINGLE:
		lgr_type = "SINGLE";
		break;
	case SMC_LGR_SYMMETRIC:
		lgr_type = "SYMMETRIC";
		break;
	case SMC_LGR_ASYMMETRIC_PEER:
		lgr_type = "ASYMMETRIC_PEER";
		break;
	case SMC_LGR_ASYMMETRIC_LOCAL:
		lgr_type = "ASYMMETRIC_LOCAL";
		break;
	}
	pr_warn_ratelimited("smc: SMC-R lg %*phN net %llu state changed: "
			    "%s, pnetid %.16s\n", SMC_LGR_ID_SIZE, &lgr->id,
			    lgr->net->net_cookie, lgr_type, lgr->pnet_id);
}

/* set new lgr type and tag a link as asymmetric */
void smcr_lgr_set_type_asym(struct smc_link_group *lgr,
			    enum smc_lgr_type new_type, int asym_lnk_idx)
{
	smcr_lgr_set_type(lgr, new_type);
	lgr->lnk[asym_lnk_idx].link_is_asym = true;
}

/* abort connection, abort_work scheduled from tasklet context */
static void smc_conn_abort_work(struct work_struct *work)
{
	struct smc_connection *conn = container_of(work,
						   struct smc_connection,
						   abort_work);
	struct smc_sock *smc = container_of(conn, struct smc_sock, conn);

	lock_sock(&smc->sk);
	smc_conn_kill(conn, true);
	release_sock(&smc->sk);
	sock_put(&smc->sk); /* sock_hold done by schedulers of abort_work */
}

void smcr_port_add(struct smc_ib_device *smcibdev, u8 ibport)
{
	struct smc_link_group *lgr, *n;

	spin_lock_bh(&smc_lgr_list.lock);
	list_for_each_entry_safe(lgr, n, &smc_lgr_list.list, list) {
		struct smc_link *link;

		if (strncmp(smcibdev->pnetid[ibport - 1], lgr->pnet_id,
			    SMC_MAX_PNETID_LEN) ||
		    lgr->type == SMC_LGR_SYMMETRIC ||
		    lgr->type == SMC_LGR_ASYMMETRIC_PEER ||
		    !rdma_dev_access_netns(smcibdev->ibdev, lgr->net))
			continue;

		if (lgr->type == SMC_LGR_SINGLE && lgr->max_links <= 1)
			continue;

		/* trigger local add link processing */
		link = smc_llc_usable_link(lgr);
		if (link)
			smc_llc_add_link_local(link);
	}
	spin_unlock_bh(&smc_lgr_list.lock);
}

/* link is down - switch connections to alternate link,
 * must be called under lgr->llc_conf_mutex lock
 */
static void smcr_link_down(struct smc_link *lnk)
{
	struct smc_link_group *lgr = lnk->lgr;
	struct smc_link *to_lnk;
	int del_link_id;

	if (!lgr || lnk->state == SMC_LNK_UNUSED || list_empty(&lgr->list))
		return;

	to_lnk = smc_switch_conns(lgr, lnk, true);
	if (!to_lnk) { /* no backup link available */
		smcr_link_clear(lnk, true);
		return;
	}
	smcr_lgr_set_type(lgr, SMC_LGR_SINGLE);
	del_link_id = lnk->link_id;

	if (lgr->role == SMC_SERV) {
		/* trigger local delete link processing */
		smc_llc_srv_delete_link_local(to_lnk, del_link_id);
	} else {
		if (lgr->llc_flow_lcl.type != SMC_LLC_FLOW_NONE) {
			/* another llc task is ongoing */
			up_write(&lgr->llc_conf_mutex);
			wait_event_timeout(lgr->llc_flow_waiter,
				(list_empty(&lgr->list) ||
				 lgr->llc_flow_lcl.type == SMC_LLC_FLOW_NONE),
				SMC_LLC_WAIT_TIME);
			down_write(&lgr->llc_conf_mutex);
		}
		if (!list_empty(&lgr->list)) {
			smc_llc_send_delete_link(to_lnk, del_link_id,
						 SMC_LLC_REQ, true,
						 SMC_LLC_DEL_LOST_PATH);
			smcr_link_clear(lnk, true);
		}
		wake_up(&lgr->llc_flow_waiter);	/* wake up next waiter */
	}
}

/* must be called under lgr->llc_conf_mutex lock */
void smcr_link_down_cond(struct smc_link *lnk)
{
	if (smc_link_downing(&lnk->state)) {
		trace_smcr_link_down(lnk, __builtin_return_address(0));
		smcr_link_down(lnk);
	}
}

/* will get the lgr->llc_conf_mutex lock */
void smcr_link_down_cond_sched(struct smc_link *lnk)
{
	if (smc_link_downing(&lnk->state)) {
		trace_smcr_link_down(lnk, __builtin_return_address(0));
		smcr_link_hold(lnk); /* smcr_link_put in link_down_wrk */
		if (!schedule_work(&lnk->link_down_wrk))
			smcr_link_put(lnk);
	}
}

void smcr_port_err(struct smc_ib_device *smcibdev, u8 ibport)
{
	struct smc_link_group *lgr, *n;
	int i;

	list_for_each_entry_safe(lgr, n, &smc_lgr_list.list, list) {
		if (strncmp(smcibdev->pnetid[ibport - 1], lgr->pnet_id,
			    SMC_MAX_PNETID_LEN))
			continue; /* lgr is not affected */
		if (list_empty(&lgr->list))
			continue;
		for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
			struct smc_link *lnk = &lgr->lnk[i];

			if (smc_link_usable(lnk) &&
			    lnk->smcibdev == smcibdev && lnk->ibport == ibport)
				smcr_link_down_cond_sched(lnk);
		}
	}
}

static void smc_link_down_work(struct work_struct *work)
{
	struct smc_link *link = container_of(work, struct smc_link,
					     link_down_wrk);
	struct smc_link_group *lgr = link->lgr;

	if (list_empty(&lgr->list))
		goto out;
	wake_up_all(&lgr->llc_msg_waiter);
	down_write(&lgr->llc_conf_mutex);
	smcr_link_down(link);
	up_write(&lgr->llc_conf_mutex);

out:
	smcr_link_put(link); /* smcr_link_hold by schedulers of link_down_work */
}

static int smc_vlan_by_tcpsk_walk(struct net_device *lower_dev,
				  struct netdev_nested_priv *priv)
{
	unsigned short *vlan_id = (unsigned short *)priv->data;

	if (is_vlan_dev(lower_dev)) {
		*vlan_id = vlan_dev_vlan_id(lower_dev);
		return 1;
	}

	return 0;
}

/* Determine vlan of internal TCP socket. */
int smc_vlan_by_tcpsk(struct socket *clcsock, struct smc_init_info *ini)
{
	struct dst_entry *dst = sk_dst_get(clcsock->sk);
	struct netdev_nested_priv priv;
	struct net_device *ndev;
	int rc = 0;

	ini->vlan_id = 0;
	if (!dst) {
		rc = -ENOTCONN;
		goto out;
	}
	if (!dst->dev) {
		rc = -ENODEV;
		goto out_rel;
	}

	ndev = dst->dev;
	if (is_vlan_dev(ndev)) {
		ini->vlan_id = vlan_dev_vlan_id(ndev);
		goto out_rel;
	}

	priv.data = (void *)&ini->vlan_id;
	rtnl_lock();
	netdev_walk_all_lower_dev(ndev, smc_vlan_by_tcpsk_walk, &priv);
	rtnl_unlock();

out_rel:
	dst_release(dst);
out:
	return rc;
}

static bool smcr_lgr_match(struct smc_link_group *lgr, u8 smcr_version,
			   u8 peer_systemid[],
			   u8 peer_gid[],
			   u8 peer_mac_v1[],
			   enum smc_lgr_role role, u32 clcqpn,
			   struct net *net)
{
	struct smc_link *lnk;
	int i;

	if (memcmp(lgr->peer_systemid, peer_systemid, SMC_SYSTEMID_LEN) ||
	    lgr->role != role)
		return false;

	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		lnk = &lgr->lnk[i];

		if (!smc_link_active(lnk))
			continue;
		/* use verbs API to check netns, instead of lgr->net */
		if (!rdma_dev_access_netns(lnk->smcibdev->ibdev, net))
			return false;
		if ((lgr->role == SMC_SERV || lnk->peer_qpn == clcqpn) &&
		    !memcmp(lnk->peer_gid, peer_gid, SMC_GID_SIZE) &&
		    (smcr_version == SMC_V2 ||
		     !memcmp(lnk->peer_mac, peer_mac_v1, ETH_ALEN)))
			return true;
	}
	return false;
}

static bool smcd_lgr_match(struct smc_link_group *lgr,
			   struct smcd_dev *smcismdev,
			   struct smcd_gid *peer_gid)
{
	if (lgr->peer_gid.gid != peer_gid->gid ||
	    lgr->smcd != smcismdev)
		return false;

	if (smc_ism_is_emulated(smcismdev) &&
	    lgr->peer_gid.gid_ext != peer_gid->gid_ext)
		return false;

	return true;
}

/* create a new SMC connection (and a new link group if necessary) */
int smc_conn_create(struct smc_sock *smc, struct smc_init_info *ini)
{
	struct smc_connection *conn = &smc->conn;
	struct net *net = sock_net(&smc->sk);
	struct list_head *lgr_list;
	struct smc_link_group *lgr;
	enum smc_lgr_role role;
	spinlock_t *lgr_lock;
	int rc = 0;

	lgr_list = ini->is_smcd ? &ini->ism_dev[ini->ism_selected]->lgr_list :
				  &smc_lgr_list.list;
	lgr_lock = ini->is_smcd ? &ini->ism_dev[ini->ism_selected]->lgr_lock :
				  &smc_lgr_list.lock;
	ini->first_contact_local = 1;
	role = smc->listen_smc ? SMC_SERV : SMC_CLNT;
	if (role == SMC_CLNT && ini->first_contact_peer)
		/* create new link group as well */
		goto create;

	/* determine if an existing link group can be reused */
	spin_lock_bh(lgr_lock);
	list_for_each_entry(lgr, lgr_list, list) {
		write_lock_bh(&lgr->conns_lock);
		if ((ini->is_smcd ?
		     smcd_lgr_match(lgr, ini->ism_dev[ini->ism_selected],
				    &ini->ism_peer_gid[ini->ism_selected]) :
		     smcr_lgr_match(lgr, ini->smcr_version,
				    ini->peer_systemid,
				    ini->peer_gid, ini->peer_mac, role,
				    ini->ib_clcqpn, net)) &&
		    !lgr->sync_err &&
		    (ini->smcd_version == SMC_V2 ||
		     lgr->vlan_id == ini->vlan_id) &&
		    (role == SMC_CLNT || ini->is_smcd ||
		    (lgr->conns_num < lgr->max_conns &&
		      !bitmap_full(lgr->rtokens_used_mask, SMC_RMBS_PER_LGR_MAX)))) {
			/* link group found */
			ini->first_contact_local = 0;
			conn->lgr = lgr;
			rc = smc_lgr_register_conn(conn, false);
			write_unlock_bh(&lgr->conns_lock);
			if (!rc && delayed_work_pending(&lgr->free_work))
				cancel_delayed_work(&lgr->free_work);
			break;
		}
		write_unlock_bh(&lgr->conns_lock);
	}
	spin_unlock_bh(lgr_lock);
	if (rc)
		return rc;

	if (role == SMC_CLNT && !ini->first_contact_peer &&
	    ini->first_contact_local) {
		/* Server reuses a link group, but Client wants to start
		 * a new one
		 * send out_of_sync decline, reason synchr. error
		 */
		return SMC_CLC_DECL_SYNCERR;
	}

create:
	if (ini->first_contact_local) {
		rc = smc_lgr_create(smc, ini);
		if (rc)
			goto out;
		lgr = conn->lgr;
		write_lock_bh(&lgr->conns_lock);
		rc = smc_lgr_register_conn(conn, true);
		write_unlock_bh(&lgr->conns_lock);
		if (rc) {
			smc_lgr_cleanup_early(lgr);
			goto out;
		}
	}
	smc_lgr_hold(conn->lgr); /* lgr_put in smc_conn_free() */
	if (!conn->lgr->is_smcd)
		smcr_link_hold(conn->lnk); /* link_put in smc_conn_free() */
	conn->freed = 0;
	conn->local_tx_ctrl.common.type = SMC_CDC_MSG_TYPE;
	conn->local_tx_ctrl.len = SMC_WR_TX_SIZE;
	conn->urg_state = SMC_URG_READ;
	init_waitqueue_head(&conn->cdc_pend_tx_wq);
	INIT_WORK(&smc->conn.abort_work, smc_conn_abort_work);
	if (ini->is_smcd) {
		conn->rx_off = sizeof(struct smcd_cdc_msg);
		smcd_cdc_rx_init(conn); /* init tasklet for this conn */
	} else {
		conn->rx_off = 0;
	}
#ifndef KERNEL_HAS_ATOMIC64
	spin_lock_init(&conn->acurs_lock);
#endif

out:
	return rc;
}

#define SMCD_DMBE_SIZES		6 /* 0 -> 16KB, 1 -> 32KB, .. 6 -> 1MB */
#define SMCR_RMBE_SIZES		15 /* 0 -> 16KB, 1 -> 32KB, .. 15 -> 512MB */

/* convert the RMB size into the compressed notation (minimum 16K, see
 * SMCD/R_DMBE_SIZES.
 * In contrast to plain ilog2, this rounds towards the next power of 2,
 * so the socket application gets at least its desired sndbuf / rcvbuf size.
 */
static u8 smc_compress_bufsize(int size, bool is_smcd, bool is_rmb)
{
	u8 compressed;

	if (size <= SMC_BUF_MIN_SIZE)
		return 0;

	size = (size - 1) >> 14;  /* convert to 16K multiple */
	compressed = min_t(u8, ilog2(size) + 1,
			   is_smcd ? SMCD_DMBE_SIZES : SMCR_RMBE_SIZES);

#ifdef CONFIG_ARCH_NO_SG_CHAIN
	if (!is_smcd && is_rmb)
		/* RMBs are backed by & limited to max size of scatterlists */
		compressed = min_t(u8, compressed, ilog2((SG_MAX_SINGLE_ALLOC * PAGE_SIZE) >> 14));
#endif

	return compressed;
}

/* convert the RMB size from compressed notation into integer */
int smc_uncompress_bufsize(u8 compressed)
{
	u32 size;

	size = 0x00000001 << (((int)compressed) + 14);
	return (int)size;
}

/* try to reuse a sndbuf or rmb description slot for a certain
 * buffer size; if not available, return NULL
 */
static struct smc_buf_desc *smc_buf_get_slot(struct rw_semaphore *lock,
					     struct list_head *buf_list)
{
	struct smc_buf_desc *buf_slot;

	down_read(lock);
	list_for_each_entry(buf_slot, buf_list, list) {
		if (cmpxchg(&buf_slot->used, 0, 1) == 0) {
			up_read(lock);
			return buf_slot;
		}
	}
	up_read(lock);
	return NULL;
}

/* one of the conditions for announcing a receiver's current window size is
 * that it "results in a minimum increase in the window size of 10% of the
 * receive buffer space" [RFC7609]
 */
static inline int smc_rmb_wnd_update_limit(int rmbe_size)
{
	return max_t(int, rmbe_size / 10, SOCK_MIN_SNDBUF / 2);
}

/* map an buf to a link */
static int smcr_buf_map_link(struct smc_buf_desc *buf_desc, bool is_rmb,
			     struct smc_link *lnk)
{
	int rc, i, nents, offset, buf_size, size, access_flags;
	struct scatterlist *sg;
	void *buf;

	if (buf_desc->is_map_ib[lnk->link_idx])
		return 0;

	if (buf_desc->is_vm) {
		buf = buf_desc->cpu_addr;
		buf_size = buf_desc->len;
		offset = offset_in_page(buf_desc->cpu_addr);
		nents = PAGE_ALIGN(buf_size + offset) / PAGE_SIZE;
	} else {
		nents = 1;
	}

	rc = sg_alloc_table(&buf_desc->sgt[lnk->link_idx], nents, GFP_KERNEL);
	if (rc)
		return rc;

	if (buf_desc->is_vm) {
		/* virtually contiguous buffer */
		for_each_sg(buf_desc->sgt[lnk->link_idx].sgl, sg, nents, i) {
			size = min_t(int, PAGE_SIZE - offset, buf_size);
			sg_set_page(sg, vmalloc_to_page(buf), size, offset);
			buf += size;
			buf_size -= size;
			offset = 0;
		}
	} else {
		/* physically contiguous buffer */
		sg_set_buf(buf_desc->sgt[lnk->link_idx].sgl,
			   buf_desc->cpu_addr, buf_desc->len);
	}

	/* map sg table to DMA address */
	rc = smc_ib_buf_map_sg(lnk, buf_desc,
			       is_rmb ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
	/* SMC protocol depends on mapping to one DMA address only */
	if (rc != nents) {
		rc = -EAGAIN;
		goto free_table;
	}

	buf_desc->is_dma_need_sync |=
		smc_ib_is_sg_need_sync(lnk, buf_desc) << lnk->link_idx;

	if (is_rmb || buf_desc->is_vm) {
		/* create a new memory region for the RMB or vzalloced sndbuf */
		access_flags = is_rmb ?
			       IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE :
			       IB_ACCESS_LOCAL_WRITE;

		rc = smc_ib_get_memory_region(lnk->roce_pd, access_flags,
					      buf_desc, lnk->link_idx);
		if (rc)
			goto buf_unmap;
		smc_ib_sync_sg_for_device(lnk, buf_desc,
					  is_rmb ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
	}
	buf_desc->is_map_ib[lnk->link_idx] = true;
	return 0;

buf_unmap:
	smc_ib_buf_unmap_sg(lnk, buf_desc,
			    is_rmb ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
free_table:
	sg_free_table(&buf_desc->sgt[lnk->link_idx]);
	return rc;
}

/* register a new buf on IB device, rmb or vzalloced sndbuf
 * must be called under lgr->llc_conf_mutex lock
 */
int smcr_link_reg_buf(struct smc_link *link, struct smc_buf_desc *buf_desc)
{
	if (list_empty(&link->lgr->list))
		return -ENOLINK;
	if (!buf_desc->is_reg_mr[link->link_idx]) {
		/* register memory region for new buf */
		if (buf_desc->is_vm)
			buf_desc->mr[link->link_idx]->iova =
				(uintptr_t)buf_desc->cpu_addr;
		if (smc_wr_reg_send(link, buf_desc->mr[link->link_idx])) {
			buf_desc->is_reg_err = true;
			return -EFAULT;
		}
		buf_desc->is_reg_mr[link->link_idx] = true;
	}
	return 0;
}

static int _smcr_buf_map_lgr(struct smc_link *lnk, struct rw_semaphore *lock,
			     struct list_head *lst, bool is_rmb)
{
	struct smc_buf_desc *buf_desc, *bf;
	int rc = 0;

	down_write(lock);
	list_for_each_entry_safe(buf_desc, bf, lst, list) {
		if (!buf_desc->used)
			continue;
		rc = smcr_buf_map_link(buf_desc, is_rmb, lnk);
		if (rc)
			goto out;
	}
out:
	up_write(lock);
	return rc;
}

/* map all used buffers of lgr for a new link */
int smcr_buf_map_lgr(struct smc_link *lnk)
{
	struct smc_link_group *lgr = lnk->lgr;
	int i, rc = 0;

	for (i = 0; i < SMC_RMBE_SIZES; i++) {
		rc = _smcr_buf_map_lgr(lnk, &lgr->rmbs_lock,
				       &lgr->rmbs[i], true);
		if (rc)
			return rc;
		rc = _smcr_buf_map_lgr(lnk, &lgr->sndbufs_lock,
				       &lgr->sndbufs[i], false);
		if (rc)
			return rc;
	}
	return 0;
}

/* register all used buffers of lgr for a new link,
 * must be called under lgr->llc_conf_mutex lock
 */
int smcr_buf_reg_lgr(struct smc_link *lnk)
{
	struct smc_link_group *lgr = lnk->lgr;
	struct smc_buf_desc *buf_desc, *bf;
	int i, rc = 0;

	/* reg all RMBs for a new link */
	down_write(&lgr->rmbs_lock);
	for (i = 0; i < SMC_RMBE_SIZES; i++) {
		list_for_each_entry_safe(buf_desc, bf, &lgr->rmbs[i], list) {
			if (!buf_desc->used)
				continue;
			rc = smcr_link_reg_buf(lnk, buf_desc);
			if (rc) {
				up_write(&lgr->rmbs_lock);
				return rc;
			}
		}
	}
	up_write(&lgr->rmbs_lock);

	if (lgr->buf_type == SMCR_PHYS_CONT_BUFS)
		return rc;

	/* reg all vzalloced sndbufs for a new link */
	down_write(&lgr->sndbufs_lock);
	for (i = 0; i < SMC_RMBE_SIZES; i++) {
		list_for_each_entry_safe(buf_desc, bf, &lgr->sndbufs[i], list) {
			if (!buf_desc->used || !buf_desc->is_vm)
				continue;
			rc = smcr_link_reg_buf(lnk, buf_desc);
			if (rc) {
				up_write(&lgr->sndbufs_lock);
				return rc;
			}
		}
	}
	up_write(&lgr->sndbufs_lock);
	return rc;
}

static struct smc_buf_desc *smcr_new_buf_create(struct smc_link_group *lgr,
						int bufsize)
{
	struct smc_buf_desc *buf_desc;

	/* try to alloc a new buffer */
	buf_desc = kzalloc(sizeof(*buf_desc), GFP_KERNEL);
	if (!buf_desc)
		return ERR_PTR(-ENOMEM);

	switch (lgr->buf_type) {
	case SMCR_PHYS_CONT_BUFS:
	case SMCR_MIXED_BUFS:
		buf_desc->order = get_order(bufsize);
		buf_desc->pages = alloc_pages(GFP_KERNEL | __GFP_NOWARN |
					      __GFP_NOMEMALLOC | __GFP_COMP |
					      __GFP_NORETRY | __GFP_ZERO,
					      buf_desc->order);
		if (buf_desc->pages) {
			buf_desc->cpu_addr =
				(void *)page_address(buf_desc->pages);
			buf_desc->len = bufsize;
			buf_desc->is_vm = false;
			break;
		}
		if (lgr->buf_type == SMCR_PHYS_CONT_BUFS)
			goto out;
		fallthrough;	// try virtually contiguous buf
	case SMCR_VIRT_CONT_BUFS:
		buf_desc->order = get_order(bufsize);
		buf_desc->cpu_addr = vzalloc(PAGE_SIZE << buf_desc->order);
		if (!buf_desc->cpu_addr)
			goto out;
		buf_desc->pages = NULL;
		buf_desc->len = bufsize;
		buf_desc->is_vm = true;
		break;
	}
	return buf_desc;

out:
	kfree(buf_desc);
	return ERR_PTR(-EAGAIN);
}

/* map buf_desc on all usable links,
 * unused buffers stay mapped as long as the link is up
 */
static int smcr_buf_map_usable_links(struct smc_link_group *lgr,
				     struct smc_buf_desc *buf_desc, bool is_rmb)
{
	int i, rc = 0, cnt = 0;

	/* protect against parallel link reconfiguration */
	down_read(&lgr->llc_conf_mutex);
	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		struct smc_link *lnk = &lgr->lnk[i];

		if (!smc_link_usable(lnk))
			continue;
		if (smcr_buf_map_link(buf_desc, is_rmb, lnk)) {
			rc = -ENOMEM;
			goto out;
		}
		cnt++;
	}
out:
	up_read(&lgr->llc_conf_mutex);
	if (!rc && !cnt)
		rc = -EINVAL;
	return rc;
}

static struct smc_buf_desc *smcd_new_buf_create(struct smc_link_group *lgr,
						bool is_dmb, int bufsize)
{
	struct smc_buf_desc *buf_desc;
	int rc;

	/* try to alloc a new DMB */
	buf_desc = kzalloc(sizeof(*buf_desc), GFP_KERNEL);
	if (!buf_desc)
		return ERR_PTR(-ENOMEM);
	if (is_dmb) {
		rc = smc_ism_register_dmb(lgr, bufsize, buf_desc);
		if (rc) {
			kfree(buf_desc);
			if (rc == -ENOMEM)
				return ERR_PTR(-EAGAIN);
			if (rc == -ENOSPC)
				return ERR_PTR(-ENOSPC);
			return ERR_PTR(-EIO);
		}
		buf_desc->pages = virt_to_page(buf_desc->cpu_addr);
		/* CDC header stored in buf. So, pretend it was smaller */
		buf_desc->len = bufsize - sizeof(struct smcd_cdc_msg);
	} else {
		buf_desc->cpu_addr = kzalloc(bufsize, GFP_KERNEL |
					     __GFP_NOWARN | __GFP_NORETRY |
					     __GFP_NOMEMALLOC);
		if (!buf_desc->cpu_addr) {
			kfree(buf_desc);
			return ERR_PTR(-EAGAIN);
		}
		buf_desc->len = bufsize;
	}
	return buf_desc;
}

static int __smc_buf_create(struct smc_sock *smc, bool is_smcd, bool is_rmb)
{
	struct smc_buf_desc *buf_desc = ERR_PTR(-ENOMEM);
	struct smc_connection *conn = &smc->conn;
	struct smc_link_group *lgr = conn->lgr;
	struct list_head *buf_list;
	int bufsize, bufsize_comp;
	struct rw_semaphore *lock;	/* lock buffer list */
	bool is_dgraded = false;

	if (is_rmb)
		/* use socket recv buffer size (w/o overhead) as start value */
		bufsize = smc->sk.sk_rcvbuf / 2;
	else
		/* use socket send buffer size (w/o overhead) as start value */
		bufsize = smc->sk.sk_sndbuf / 2;

	for (bufsize_comp = smc_compress_bufsize(bufsize, is_smcd, is_rmb);
	     bufsize_comp >= 0; bufsize_comp--) {
		if (is_rmb) {
			lock = &lgr->rmbs_lock;
			buf_list = &lgr->rmbs[bufsize_comp];
		} else {
			lock = &lgr->sndbufs_lock;
			buf_list = &lgr->sndbufs[bufsize_comp];
		}
		bufsize = smc_uncompress_bufsize(bufsize_comp);

		/* check for reusable slot in the link group */
		buf_desc = smc_buf_get_slot(lock, buf_list);
		if (buf_desc) {
			buf_desc->is_dma_need_sync = 0;
			SMC_STAT_RMB_SIZE(smc, is_smcd, is_rmb, true, bufsize);
			SMC_STAT_BUF_REUSE(smc, is_smcd, is_rmb);
			break; /* found reusable slot */
		}

		if (is_smcd)
			buf_desc = smcd_new_buf_create(lgr, is_rmb, bufsize);
		else
			buf_desc = smcr_new_buf_create(lgr, bufsize);

		if (PTR_ERR(buf_desc) == -ENOMEM)
			break;
		if (IS_ERR(buf_desc)) {
			if (!is_dgraded) {
				is_dgraded = true;
				SMC_STAT_RMB_DOWNGRADED(smc, is_smcd, is_rmb);
			}
			continue;
		}

		SMC_STAT_RMB_ALLOC(smc, is_smcd, is_rmb);
		SMC_STAT_RMB_SIZE(smc, is_smcd, is_rmb, true, bufsize);
		buf_desc->used = 1;
		down_write(lock);
		smc_lgr_buf_list_add(lgr, is_rmb, buf_list, buf_desc);
		up_write(lock);
		break; /* found */
	}

	if (IS_ERR(buf_desc))
		return PTR_ERR(buf_desc);

	if (!is_smcd) {
		if (smcr_buf_map_usable_links(lgr, buf_desc, is_rmb)) {
			smcr_buf_unuse(buf_desc, is_rmb, lgr);
			return -ENOMEM;
		}
	}

	if (is_rmb) {
		conn->rmb_desc = buf_desc;
		conn->rmbe_size_comp = bufsize_comp;
		smc->sk.sk_rcvbuf = bufsize * 2;
		atomic_set(&conn->bytes_to_rcv, 0);
		conn->rmbe_update_limit =
			smc_rmb_wnd_update_limit(buf_desc->len);
		if (is_smcd)
			smc_ism_set_conn(conn); /* map RMB/smcd_dev to conn */
	} else {
		conn->sndbuf_desc = buf_desc;
		smc->sk.sk_sndbuf = bufsize * 2;
		atomic_set(&conn->sndbuf_space, bufsize);
	}
	return 0;
}

void smc_sndbuf_sync_sg_for_device(struct smc_connection *conn)
{
	if (!conn->sndbuf_desc->is_dma_need_sync)
		return;
	if (!smc_conn_lgr_valid(conn) || conn->lgr->is_smcd ||
	    !smc_link_active(conn->lnk))
		return;
	smc_ib_sync_sg_for_device(conn->lnk, conn->sndbuf_desc, DMA_TO_DEVICE);
}

void smc_rmb_sync_sg_for_cpu(struct smc_connection *conn)
{
	int i;

	if (!conn->rmb_desc->is_dma_need_sync)
		return;
	if (!smc_conn_lgr_valid(conn) || conn->lgr->is_smcd)
		return;
	for (i = 0; i < SMC_LINKS_PER_LGR_MAX; i++) {
		if (!smc_link_active(&conn->lgr->lnk[i]))
			continue;
		smc_ib_sync_sg_for_cpu(&conn->lgr->lnk[i], conn->rmb_desc,
				       DMA_FROM_DEVICE);
	}
}

/* create the send and receive buffer for an SMC socket;
 * receive buffers are called RMBs;
 * (even though the SMC protocol allows more than one RMB-element per RMB,
 * the Linux implementation uses just one RMB-element per RMB, i.e. uses an
 * extra RMB for every connection in a link group
 */
int smc_buf_create(struct smc_sock *smc, bool is_smcd)
{
	int rc;

	/* create send buffer */
	if (is_smcd &&
	    smc_ism_support_dmb_nocopy(smc->conn.lgr->smcd))
		goto create_rmb;

	rc = __smc_buf_create(smc, is_smcd, false);
	if (rc)
		return rc;

create_rmb:
	/* create rmb */
	rc = __smc_buf_create(smc, is_smcd, true);
	if (rc && smc->conn.sndbuf_desc) {
		down_write(&smc->conn.lgr->sndbufs_lock);
		smc_lgr_buf_list_del(smc->conn.lgr, false,
				     smc->conn.sndbuf_desc);
		up_write(&smc->conn.lgr->sndbufs_lock);
		smc_buf_free(smc->conn.lgr, false, smc->conn.sndbuf_desc);
		smc->conn.sndbuf_desc = NULL;
	}
	return rc;
}

int smcd_buf_attach(struct smc_sock *smc)
{
	struct smc_connection *conn = &smc->conn;
	struct smcd_dev *smcd = conn->lgr->smcd;
	u64 peer_token = conn->peer_token;
	struct smc_buf_desc *buf_desc;
	int rc;

	buf_desc = kzalloc(sizeof(*buf_desc), GFP_KERNEL);
	if (!buf_desc)
		return -ENOMEM;

	/* The ghost sndbuf_desc describes the same memory region as
	 * peer RMB. Its lifecycle is consistent with the connection's
	 * and it will be freed with the connections instead of the
	 * link group.
	 */
	rc = smc_ism_attach_dmb(smcd, peer_token, buf_desc);
	if (rc)
		goto free;

	smc->sk.sk_sndbuf = buf_desc->len;
	buf_desc->cpu_addr =
		(u8 *)buf_desc->cpu_addr + sizeof(struct smcd_cdc_msg);
	buf_desc->len -= sizeof(struct smcd_cdc_msg);
	conn->sndbuf_desc = buf_desc;
	conn->sndbuf_desc->used = 1;
	atomic_set(&conn->sndbuf_space, conn->sndbuf_desc->len);
	return 0;

free:
	kfree(buf_desc);
	return rc;
}

static inline int smc_rmb_reserve_rtoken_idx(struct smc_link_group *lgr)
{
	int i;

	for_each_clear_bit(i, lgr->rtokens_used_mask, SMC_RMBS_PER_LGR_MAX) {
		if (!test_and_set_bit(i, lgr->rtokens_used_mask))
			return i;
	}
	return -ENOSPC;
}

static int smc_rtoken_find_by_link(struct smc_link_group *lgr, int lnk_idx,
				   u32 rkey)
{
	int i;

	for (i = 0; i < SMC_RMBS_PER_LGR_MAX; i++) {
		if (test_bit(i, lgr->rtokens_used_mask) &&
		    lgr->rtokens[i][lnk_idx].rkey == rkey)
			return i;
	}
	return -ENOENT;
}

/* set rtoken for a new link to an existing rmb */
void smc_rtoken_set(struct smc_link_group *lgr, int link_idx, int link_idx_new,
		    __be32 nw_rkey_known, __be64 nw_vaddr, __be32 nw_rkey)
{
	int rtok_idx;

	rtok_idx = smc_rtoken_find_by_link(lgr, link_idx, ntohl(nw_rkey_known));
	if (rtok_idx == -ENOENT)
		return;
	lgr->rtokens[rtok_idx][link_idx_new].rkey = ntohl(nw_rkey);
	lgr->rtokens[rtok_idx][link_idx_new].dma_addr = be64_to_cpu(nw_vaddr);
}

/* set rtoken for a new link whose link_id is given */
void smc_rtoken_set2(struct smc_link_group *lgr, int rtok_idx, int link_id,
		     __be64 nw_vaddr, __be32 nw_rkey)
{
	u64 dma_addr = be64_to_cpu(nw_vaddr);
	u32 rkey = ntohl(nw_rkey);
	bool found = false;
	int link_idx;

	for (link_idx = 0; link_idx < SMC_LINKS_PER_LGR_MAX; link_idx++) {
		if (lgr->lnk[link_idx].link_id == link_id) {
			found = true;
			break;
		}
	}
	if (!found)
		return;
	lgr->rtokens[rtok_idx][link_idx].rkey = rkey;
	lgr->rtokens[rtok_idx][link_idx].dma_addr = dma_addr;
}

/* add a new rtoken from peer */
int smc_rtoken_add(struct smc_link *lnk, __be64 nw_vaddr, __be32 nw_rkey)
{
	struct smc_link_group *lgr = smc_get_lgr(lnk);
	u64 dma_addr = be64_to_cpu(nw_vaddr);
	u32 rkey = ntohl(nw_rkey);
	int i;

	for (i = 0; i < SMC_RMBS_PER_LGR_MAX; i++) {
		if (lgr->rtokens[i][lnk->link_idx].rkey == rkey &&
		    lgr->rtokens[i][lnk->link_idx].dma_addr == dma_addr &&
		    test_bit(i, lgr->rtokens_used_mask)) {
			/* already in list */
			return i;
		}
	}
	i = smc_rmb_reserve_rtoken_idx(lgr);
	if (i < 0)
		return i;
	lgr->rtokens[i][lnk->link_idx].rkey = rkey;
	lgr->rtokens[i][lnk->link_idx].dma_addr = dma_addr;
	return i;
}

/* delete an rtoken from all links */
int smc_rtoken_delete(struct smc_link *lnk, __be32 nw_rkey)
{
	struct smc_link_group *lgr = smc_get_lgr(lnk);
	u32 rkey = ntohl(nw_rkey);
	int i, j;

	for (i = 0; i < SMC_RMBS_PER_LGR_MAX; i++) {
		if (lgr->rtokens[i][lnk->link_idx].rkey == rkey &&
		    test_bit(i, lgr->rtokens_used_mask)) {
			for (j = 0; j < SMC_LINKS_PER_LGR_MAX; j++) {
				lgr->rtokens[i][j].rkey = 0;
				lgr->rtokens[i][j].dma_addr = 0;
			}
			clear_bit(i, lgr->rtokens_used_mask);
			return 0;
		}
	}
	return -ENOENT;
}

/* save rkey and dma_addr received from peer during clc handshake */
int smc_rmb_rtoken_handling(struct smc_connection *conn,
			    struct smc_link *lnk,
			    struct smc_clc_msg_accept_confirm *clc)
{
	conn->rtoken_idx = smc_rtoken_add(lnk, clc->r0.rmb_dma_addr,
					  clc->r0.rmb_rkey);
	if (conn->rtoken_idx < 0)
		return conn->rtoken_idx;
	return 0;
}

static void smc_core_going_away(void)
{
	struct smc_ib_device *smcibdev;
	struct smcd_dev *smcd;

	mutex_lock(&smc_ib_devices.mutex);
	list_for_each_entry(smcibdev, &smc_ib_devices.list, list) {
		int i;

		for (i = 0; i < SMC_MAX_PORTS; i++)
			set_bit(i, smcibdev->ports_going_away);
	}
	mutex_unlock(&smc_ib_devices.mutex);

	mutex_lock(&smcd_dev_list.mutex);
	list_for_each_entry(smcd, &smcd_dev_list.list, list) {
		smcd->going_away = 1;
	}
	mutex_unlock(&smcd_dev_list.mutex);
}

/* Clean up all SMC link groups */
static void smc_lgrs_shutdown(void)
{
	struct smcd_dev *smcd;

	smc_core_going_away();

	smc_smcr_terminate_all(NULL);

	mutex_lock(&smcd_dev_list.mutex);
	list_for_each_entry(smcd, &smcd_dev_list.list, list)
		smc_smcd_terminate_all(smcd);
	mutex_unlock(&smcd_dev_list.mutex);
}

static int smc_core_reboot_event(struct notifier_block *this,
				 unsigned long event, void *ptr)
{
	smc_lgrs_shutdown();
	smc_ib_unregister_client();
	smc_ism_exit();
	return 0;
}

static struct notifier_block smc_reboot_notifier = {
	.notifier_call = smc_core_reboot_event,
};

int __init smc_core_init(void)
{
	return register_reboot_notifier(&smc_reboot_notifier);
}

/* Called (from smc_exit) when module is removed */
void smc_core_exit(void)
{
	unregister_reboot_notifier(&smc_reboot_notifier);
	smc_lgrs_shutdown();
}
