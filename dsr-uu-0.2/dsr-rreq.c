/* Copyright (C) Uppsala University
 *
 * This file is distributed under the terms of the GNU general Public
 * License (GPL), see the file LICENSE
 *
 * Author: Erik Nordström, <erikn@it.uu.se>
 */
#ifdef __KERNEL__
#include <linux/proc_fs.h>
#include <linux/timer.h>
#include <net/ip.h>
#include <linux/random.h>

#include "dsr-dev.h"
#endif

#ifdef NS2
#include "ns-agent.h"
#endif

#include "debug.h"
#include "dsr.h"
#include "tbl.h"
#include "dsr-rrep.h"
#include "dsr-rreq.h"
#include "dsr-opt.h"
#include "link-cache.h"
#include "send-buf.h"
#include "neigh.h"

#ifndef NS2

#define RREQ_TBL_PROC_NAME "dsr_rreq_tbl"

static TBL(rreq_tbl, RREQ_TBL_MAX_LEN);
static unsigned int rreq_seqno;
#endif

#ifndef MAXTTL
#define MAXTTL 255
#endif

#define STATE_IDLE          0
#define STATE_IN_ROUTE_DISC 1
//定义路由请求表
struct rreq_tbl_entry {
	list_t l;
	int state;               //表示路由请求的状态
	struct in_addr node_addr;//路由请求的目标地址
	int ttl;
	DSRUUTimer *timer;
	struct timeval tx_time;
	struct timeval last_used;//上一次被使用到现在的时间间隔
	usecs_t timeout;
	unsigned int num_rexmts;
	struct tbl rreq_id_tbl;
};

struct id_entry {
	list_t l;
	struct in_addr trg_addr;
	unsigned short id;
};
struct rreq_tbl_query {
	struct in_addr *initiator;
	struct in_addr *target;
	unsigned int *id;
};
//typedef int (*criteria_t) (void *elm, void *data);crit_addr-判断路由请求表项的目的地址和传进来参数的地址是否匹配
static inline int crit_addr(void *pos, void *data)
{//crit_addr-判断路由请求表项的目的地址和传进来参数的地址是否匹配
	struct rreq_tbl_entry *e = (struct rreq_tbl_entry *)pos;
	struct in_addr *a = (struct in_addr *)data;

	if (e->node_addr.s_addr == a->s_addr)
		return 1;
	return 0;
}

static inline int crit_duplicate(void *pos, void *data)
{
	struct rreq_tbl_entry *e = (struct rreq_tbl_entry *)pos;
	struct rreq_tbl_query *q = (struct rreq_tbl_query *)data;

	if (e->node_addr.s_addr == q->initiator->s_addr) {
		list_t *p;

		list_for_each(p, &e->rreq_id_tbl.head) {
			struct id_entry *id_e = (struct id_entry *)p;

			if (id_e->trg_addr.s_addr == q->target->s_addr &&
			    id_e->id == *(q->id))
				return 1;
		}
	}
	return 0;
}

void NSCLASS rreq_tbl_set_max_len(unsigned int max_len)
{
	rreq_tbl.max_len = max_len;
}
#ifdef __KERNEL__
static int rreq_tbl_print(struct tbl *t, char *buf)//路由表输出功能定义
{
	list_t *pos1, *pos2;
	int len = 0;
	int first = 1;
	struct timeval now;

	gettime(&now);

	DSR_READ_LOCK(&t->lock);

	len +=
	    sprintf(buf, "# %-15s %-6s %-8s %15s:%s\n", "IPAddr", "TTL", "Used",
		    "TargetIPAddr", "ID");

	list_for_each(pos1, &t->head) {
		struct rreq_tbl_entry *e = (struct rreq_tbl_entry *)pos1;
		struct id_entry *id_e;

		if (TBL_EMPTY(&e->rreq_id_tbl))
			len +=
			    sprintf(buf + len, "  %-15s %-6u %-8lu %15s:%s\n",
				    print_ip(e->node_addr), e->ttl,
				    timeval_diff(&now, &e->last_used) / 1000000,
				    "-", "-");
		else {
			id_e = (struct id_entry *)TBL_FIRST(&e->rreq_id_tbl);
			len +=
			    sprintf(buf + len, "  %-15s %-6u %-8lu %15s:%u\n",
				    print_ip(e->node_addr), e->ttl,
				    timeval_diff(&now, &e->last_used) / 1000000,
				    print_ip(id_e->trg_addr), id_e->id);
		}
		list_for_each(pos2, &e->rreq_id_tbl.head) {
			id_e = (struct id_entry *)pos2;
			if (!first)
				len +=
				    sprintf(buf + len, "%49s:%u\n",
					    print_ip(id_e->trg_addr), id_e->id);
			first = 0;
		}
	}

	DSR_READ_UNLOCK(&t->lock);
	return len;

}
#endif /* __KERNEL__ */

void NSCLASS rreq_tbl_timeout(unsigned long data)//超时定义
{
	struct rreq_tbl_entry *e = (struct rreq_tbl_entry *)data;
	struct timeval expires;

	if (!e)
		return;

	tbl_detach(&rreq_tbl, &e->l);

	DEBUG("RREQ Timeout dst=%s timeout=%lu rexmts=%d \n",
	      print_ip(e->node_addr), e->timeout, e->num_rexmts);

	if (e->num_rexmts >= ConfVal(MaxRequestRexmt)) {
		DEBUG("MAX RREQs reached for %s\n", print_ip(e->node_addr));

		e->state = STATE_IDLE;

/* 		DSR_WRITE_UNLOCK(&rreq_tbl); */
		tbl_add_tail(&rreq_tbl, &e->l);
		return;
	}

	e->num_rexmts++;

	/* if (e->ttl == 1) */
/* 		e->timeout = ConfValToUsecs(RequestPeriod);  */
/* 	else */
	e->timeout *= 2;	/* Double timeout */

	e->ttl *= 2;		/* Double TTL */

	if (e->ttl > MAXTTL)
		e->ttl = MAXTTL;

	if (e->timeout > ConfValToUsecs(MaxRequestPeriod))
		e->timeout = ConfValToUsecs(MaxRequestPeriod);

	gettime(&e->last_used);

	dsr_rreq_send(e->node_addr, e->ttl);

	expires = e->last_used;
	timeval_add_usecs(&expires, e->timeout);

	/* Put at end of list */
	tbl_add_tail(&rreq_tbl, &e->l);

	set_timer(e->timer, &expires);
}
//_rreq_tbl_entry_create()函数，对表项进行内存分配，然后初始化项目值，然后返回指向路由请求表的指针。
struct rreq_tbl_entry *NSCLASS __rreq_tbl_entry_create(struct in_addr node_addr)
{
	struct rreq_tbl_entry *e;

	e = (struct rreq_tbl_entry *)MALLOC(sizeof(struct rreq_tbl_entry),
					    GFP_ATOMIC);

	if (!e)
		return NULL;

	e->state = STATE_IDLE;
	e->node_addr = node_addr;
	e->ttl = 0;
	memset(&e->tx_time, 0, sizeof(struct timeval));;
	e->num_rexmts = 0;
#ifdef NS2
	e->timer = new DSRUUTimer(this, "RREQTblTimer");
#else
	e->timer = MALLOC(sizeof(DSRUUTimer), GFP_ATOMIC);
#endif

	if (!e->timer) {
		FREE(e);
		return NULL;
	}

	init_timer(e->timer);

	e->timer->function = &NSCLASS rreq_tbl_timeout;
	e->timer->data = (unsigned long)e;

	INIT_TBL(&e->rreq_id_tbl, ConfVal(RequestTableIds));

	return e;
}

struct rreq_tbl_entry *NSCLASS __rreq_tbl_add(struct in_addr node_addr)
{
	struct rreq_tbl_entry *e;

	e = __rreq_tbl_entry_create(node_addr);
//首先会调用_rreq_tbl_entry_create()函数，对表项进行内存分配，然后初始化项目值，然后返回指向路由请求表的指针。
	if (!e)
		return NULL;

	if (TBL_FULL(&rreq_tbl)) {
		struct rreq_tbl_entry *f;

		f = (struct rreq_tbl_entry *)TBL_FIRST(&rreq_tbl);

		__tbl_detach(&rreq_tbl, &f->l);

		del_timer_sync(f->timer);
#ifdef NS2
		delete f->timer;
#else
		FREE(f->timer);
#endif
		tbl_flush(&f->rreq_id_tbl, NULL);

		FREE(f);
	}//在249-265行，会对请求表进行判断，检查表中条目是否已满，如果已经满了，就删除表中的第一条请求。
	__tbl_add_tail(&rreq_tbl, &e->l);

	return e;
}
//在266行，调用_tbl_add_tail()函数，将新的请求添加到请求表的最后，然后返回指针。
int NSCLASS
rreq_tbl_add_id(struct in_addr initiator, struct in_addr target,
		unsigned short id)
{
	struct rreq_tbl_entry *e;
	struct id_entry *id_e;
	int res = 0;

	DSR_WRITE_LOCK(&rreq_tbl.lock);

	e = (struct rreq_tbl_entry *)__tbl_find(&rreq_tbl, &initiator,
						crit_addr);

	if (!e)
		e = __rreq_tbl_add(initiator);
	else {
		/* Put it last in the table */
		__tbl_detach(&rreq_tbl, &e->l);
		__tbl_add_tail(&rreq_tbl, &e->l);
	}

	if (!e) {
		res = -ENOMEM;
		goto out;
	}

	gettime(&e->last_used);

	if (TBL_FULL(&e->rreq_id_tbl))
		tbl_del_first(&e->rreq_id_tbl);

	id_e = (struct id_entry *)MALLOC(sizeof(struct id_entry), GFP_ATOMIC);

	if (!id_e) {
		res = -ENOMEM;
		goto out;
	}

	id_e->trg_addr = target;
	id_e->id = id;

	tbl_add_tail(&e->rreq_id_tbl, &id_e->l);
      out:
	DSR_WRITE_UNLOCK(&rreq_tbl.lock);

	return 1;
}

int NSCLASS rreq_tbl_route_discovery_cancel(struct in_addr dst)
{
	struct rreq_tbl_entry *e;

	e = (struct rreq_tbl_entry *)tbl_find_detach(&rreq_tbl, &dst,
						     crit_addr);

	if (!e) {
		DEBUG("%s not in RREQ table\n", print_ip(dst));
		return -1;
	}

	if (e->state == STATE_IN_ROUTE_DISC)
		del_timer_sync(e->timer);

	e->state = STATE_IDLE;
	gettime(&e->last_used);

	tbl_add_tail(&rreq_tbl, &e->l);

	return 1;
}
//在dsr_rreq_route_discovery() 函数中，实现了发送路由请求，进行路由发现的功能
int NSCLASS dsr_rreq_route_discovery(struct in_addr target)
{//
	struct rreq_tbl_entry *e;
	int ttl, res = 0;
	struct timeval expires;
/*首先，在344-346行定义一个指向路由请求表的指针；在路由请求表的结构体中，
存放着路由求情的目的地址，当前状态，以及关于时间的一些变量；还定义了TTL，控制缓存失效期限的变量expires*/
#define	TTL_START 1

	DSR_WRITE_LOCK(&rreq_tbl.lock);

	e = (struct rreq_tbl_entry *)__tbl_find(&rreq_tbl, &target, crit_addr);//调用_tbl_find()函数对当前目标在路由请求表中先进行搜索
//在_tbl_find()中，是对链表中的数据进行遍历，如果发现满足条件的节点，则返回指向该结点的指针，否则返回NULL
	if (!e)//若没有找到在路由请求表中相同的路由请求，那么就调用_rreq_tbl_add()函数，把target添加到路由请求表中
		e = __rreq_tbl_add(target);//可参见上面第240行是_rreq_tbl_add()函数的代码实现////////
	else {//若有，则删除原有的路由请求，将新的路由请求添加到路由请求表的末尾。
		/* Put it last in the table */
		__tbl_detach(&rreq_tbl, &e->l);
		__tbl_add_tail(&rreq_tbl, &e->l);
	}

	if (!e) {
		res = -ENOMEM;
		goto out;
	}
//检查state的值
	if (e->state == STATE_IN_ROUTE_DISC) {//表示“当前状态已经在路由发现中”
		DEBUG("Route discovery for %s already in progress\n",
		      print_ip(target));//打印信息
		goto out;//函数执行结束
	}
	DEBUG("Route discovery for %s\n", print_ip(target));
//时间相关的变量进行设置
	gettime(&e->last_used);
	e->ttl = ttl = TTL_START;
	/* The draft does not actually specify how these Request Timeout values
	 * should be used... ??? I am just guessing here. */
//草案实际上没有指定这些请求超时值的方式
	if (e->ttl == 1)
		e->timeout = ConfValToUsecs(NonpropRequestTimeout);
	else
		e->timeout = ConfValToUsecs(RequestPeriod);

	e->state = STATE_IN_ROUTE_DISC;
	e->num_rexmts = 0;

	expires = e->last_used;
	timeval_add_usecs(&expires, e->timeout);

	set_timer(e->timer, &expires);

	DSR_WRITE_UNLOCK(&rreq_tbl.lock);

	dsr_rreq_send(target, ttl);

	return 1;
      out:
	DSR_WRITE_UNLOCK(&rreq_tbl.lock);

	return res;
}
//最后返回res结束还是的执行。
int NSCLASS dsr_rreq_duplicate(struct in_addr initiator, struct in_addr target,
			       unsigned int id)
{
	struct {
		struct in_addr *initiator;
		struct in_addr *target;
		unsigned int *id;
	} d;

	d.initiator = &initiator;
	d.target = &target;
	d.id = &id;

	return in_tbl(&rreq_tbl, &d, crit_duplicate);
}
//添加路由请求的实现
static struct dsr_rreq_opt *dsr_rreq_opt_add(char *buf, unsigned int len,
					     struct in_addr target,
					     unsigned int seqno)
{
	struct dsr_rreq_opt *rreq_opt;
//判断缓冲区的空间是否足够
	if (!buf || len < DSR_RREQ_HDR_LEN)
		return NULL;
//如果足够，就把选项指针指向缓冲区，并初始化id、长度和目的地址
	rreq_opt = (struct dsr_rreq_opt *)buf;

	rreq_opt->type = DSR_OPT_RREQ;
	rreq_opt->length = 6;
	rreq_opt->id = htons(seqno);
	rreq_opt->target = target.s_addr;

	return rreq_opt;
}
//dsr_rreq_send()实现了发送路由请求的功能
int NSCLASS dsr_rreq_send(struct in_addr target, int ttl)
{
	struct dsr_pkt *dp;
	char *buf;
	int len = DSR_OPT_HDR_LEN + DSR_RREQ_HDR_LEN;

	dp = dsr_pkt_alloc(NULL);//先申请一个数据包
//然后对申请数据包的成功与否进行判断
	if (!dp) {
		DEBUG("Could not allocate DSR packet\n");
		return -1;//若失败则打印信息并退出
	}//若成功，则在451-453行，将数据包的目的地址和下一跳地址都定义成广播地址，将源地址调用my_addr()函数，设置为自身地址
	dp->dst.s_addr = DSR_BROADCAST;    //将数据包的目的地址定义成广播地址
	dp->nxt_hop.s_addr = DSR_BROADCAST;//将数据包的下一跳地址定义成广播地址
	dp->src = my_addr();               //源地址调用my_addr()函数，设置为自身地址
//然后在455-459行，为选项申请内存地址并检查是否申请成功
	buf = dsr_pkt_alloc_opts(dp, len);

//若失败，则调用dsr_pkt_free()函数，将该数据包的空间释放，丢弃该数据包，然后退出函数
	if (!buf)
		goto out_err;
//若成功，则调用dsr_build_ip()函数
	dp->nh.iph =
	    dsr_build_ip(dp, dp->src, dp->dst, IP_HDR_LEN, IP_HDR_LEN + len,
			 IPPROTO_DSR, ttl);
//构造数据包的头部，包括源端IP地址，目的端IP地址，TTL等信息
	if (!dp->nh.iph)
		goto out_err;
//此处若构造失败，则丢弃该数据包并退出函数
	dp->dh.opth = dsr_opt_hdr_add(buf, len, DSR_NO_NEXT_HDR_TYPE);
//dsr_opt_hdr_add()构造选项头，初始化信息，并检查是否构造成功
	if (!dp->dh.opth) {
		DEBUG("Could not create DSR opt header\n");
		goto out_err;
	}//若失败就打印信息并丢弃数据包
         //若成功，则调用dsr_opt_hdr_add()对路由请求选项进行添加，同样需要检查是否添加成功
	buf += DSR_OPT_HDR_LEN;
	len -= DSR_OPT_HDR_LEN;

	dp->rreq_opt = dsr_rreq_opt_add(buf, len, target, ++rreq_seqno);

	if (!dp->rreq_opt) {
		DEBUG("Could not create RREQ opt\n");
		goto out_err;
	}
#ifdef NS2
	DEBUG("Sending RREQ src=%s dst=%s target=%s ttl=%d iph->saddr()=%d\n",
	      print_ip(dp->src), print_ip(dp->dst), print_ip(target), ttl,
	      dp->nh.iph->saddr());
#endif

	dp->flags |= PKT_XMIT_JITTER;

	XMIT(dp);
//调用XMIT()函数（实际是调用dsr_dev_xmit()函数），进行数据包的发送
	return 0;

      out_err:
	dsr_pkt_free(dp);
//调用dsr_pkt_free()函数，将该数据包的空间释放，丢弃该数据包，然后退出函数
	return -1;
}
//dsr_rreq_opt_recv()函数实现了接收处理路由发现请求选项的功能
int NSCLASS dsr_rreq_opt_recv(struct dsr_pkt *dp, struct dsr_rreq_opt *rreq_opt)
{
	struct in_addr myaddr;
	struct in_addr trg;
	struct dsr_srt *srt_rev, *srt_rc;
	int action = DSR_PKT_NONE;
	int i, n;
//对数据包、选项进行判断
	if (!dp || !rreq_opt || dp->flags & PKT_PROMISC_RECV)
		return DSR_PKT_DROP;//若不满足条件，则丢弃该数据包
	
	dp->num_rreq_opts++;
	
	if (dp->num_rreq_opts > 1) {//如果发现该路由请求选项超过了一个
		DEBUG("More than one RREQ opt!!! - Ignoring\n");//则打印错误
		return DSR_PKT_ERROR;                           //并返回“包错误”标记
	}

	dp->rreq_opt = rreq_opt;

	myaddr = my_addr();
	
	trg.s_addr = rreq_opt->target;
//调用dsr_rreq_duplicate()函数，检查当前节点是否收到过该请求
	if (dsr_rreq_duplicate(dp->src, trg, ntohs(rreq_opt->id))) { //若收到过
		DEBUG("Duplicate RREQ from %s\n", print_ip(dp->src));//则打印错误
		return DSR_PKT_DROP;                                 //并丢弃该数据包
	}
 //若未重复收到，则调用rreq_tbl_add_id()函数，在路由请求表中添加该请求   
	rreq_tbl_add_id(dp->src, trg, ntohs(rreq_opt->id)); //调用rreq_tbl_add_id()函数
//调用dsr_srt_new()函数，根据当前节点的路由，建立源路由
	dp->srt = dsr_srt_new(dp->src, myaddr, DSR_RREQ_ADDRS_LEN(rreq_opt),
			      (char *)rreq_opt->addrs);//调用dsr_srt_new()函数
//然后进行判断
	if (!dp->srt) {
		DEBUG("Could not extract source route\n");
		return DSR_PKT_ERROR;
	}
	DEBUG("RREQ target=%s src=%s dst=%s laddrs=%d\n",
	      print_ip(trg), print_ip(dp->src),
	      print_ip(dp->dst), DSR_RREQ_ADDRS_LEN(rreq_opt));
//打印RREQ的信息
	/* Add reversed source route */
//调用dsr_srt_new_rev()函数，根据当前路由，寻找逆向源路由来进行路由回复
	srt_rev = dsr_srt_new_rev(dp->srt);
//接下来进行判断并打印信息
	if (!srt_rev) {
		DEBUG("Could not reverse source route\n");
		return DSR_PKT_ERROR;
	}
	DEBUG("srt: %s\n", print_srt(dp->srt));
	DEBUG("srt_rev: %s\n", print_srt(srt_rev));

	dsr_rtc_add(srt_rev, ConfValToUsecs(RouteCacheTimeout), 0);

	/* Set previous hop *///设置上一跳信息
	if (srt_rev->laddrs > 0)
		dp->prv_hop = srt_rev->addrs[0];
	else
		dp->prv_hop = srt_rev->dst;

	neigh_tbl_add(dp->prv_hop, dp->mac.ethh);

	/* Send buffered packets *///然后发送缓存包
	send_buf_set_verdict(SEND_BUF_SEND, srt_rev->dst);

	if (rreq_opt->target == myaddr.s_addr) {//如果目标节点就是当前节点的话

		DEBUG("RREQ OPT for me - Send RREP\n");//打印信息
//根据草案，IP头中的dest addr必须使用目标地址进行更新
		/* According to the draft, the dest addr in the IP header must
		 * be updated with the target address */
#ifdef NS2//然后根据是否在NS2平台进行模拟来设置
		dp->nh.iph->daddr() = (nsaddr_t) rreq_opt->target;
#else
		dp->nh.iph->daddr = rreq_opt->target;
#endif
		dsr_rrep_send(srt_rev, dp->srt);//最后发送路由回复

		action = DSR_PKT_NONE;//然后将当前状态action设置为不进行任何操作
		goto out;//释放空间，结束函数。
	} 
	
	n = DSR_RREQ_ADDRS_LEN(rreq_opt) / sizeof(struct in_addr);
	
	if (dp->srt->src.s_addr == myaddr.s_addr)
		return DSR_PKT_DROP;
	
	for (i = 0; i < n; i++)
		if (dp->srt->addrs[i].s_addr == myaddr.s_addr) {
			action = DSR_PKT_DROP;
			goto out;
		}
//如果当前节点不是目的节点的话检查黑名单
	/* TODO: Check Blacklist *///调用lc_srt_find()函数，查看自身节点中是否存在到目的节点的路由
	srt_rc = lc_srt_find(myaddr, trg);
	
	if (srt_rc) {
		struct dsr_srt *srt_cat;
		/* Send cached route reply */
		
		DEBUG("Send cached RREP\n");
//如果发现了对应的路由，调用dsr_srt_concatenate()函数将路由连接起来，然后返回路由回复
		srt_cat = dsr_srt_concatenate(dp->srt, srt_rc);
		
		FREE(srt_rc);

		if (!srt_cat) {
			DEBUG("Could not concatenate\n");
			goto rreq_forward;
		}

		DEBUG("srt_cat: %s\n", print_srt(srt_cat));
		
		if (dsr_srt_check_duplicate(srt_cat) > 0) {
			DEBUG("Duplicate address in source route!!!\n");
			FREE(srt_cat);
			goto rreq_forward;				
		}
#ifdef NS2
		dp->nh.iph->daddr() = (nsaddr_t) rreq_opt->target;
#else
		dp->nh.iph->daddr = rreq_opt->target;
#endif
		DEBUG("Sending cached RREP to %s\n", print_ip(dp->src));
		dsr_rrep_send(srt_rev, srt_cat);
		
		action = DSR_PKT_NONE;	
		
		FREE(srt_cat);
	} else {
//但如果没有发现满足要求的路由，调用dsr_pkt_alloc_opts_expand()函数，DSR选项分配更多的内存空间
	rreq_forward:	
		dsr_pkt_alloc_opts_expand(dp, sizeof(struct in_addr));

		if (!DSR_LAST_OPT(dp, rreq_opt)) {
			char *to, *from;
			to = (char *)rreq_opt + rreq_opt->length + 2 +
			    sizeof(struct in_addr);
			from = (char *)rreq_opt + rreq_opt->length + 2;

			memmove(to, from, sizeof(struct in_addr));
		}
		rreq_opt->addrs[n] = myaddr.s_addr;//然后将自身的IP添加到路由请求选项中
		rreq_opt->length += sizeof(struct in_addr);
//更新IP头信息并进行转发。
		dp->dh.opth->p_len = htons(ntohs(dp->dh.opth->p_len) +
					   sizeof(struct in_addr));
#ifdef __KERNEL__
		dsr_build_ip(dp, dp->src, dp->dst, IP_HDR_LEN,
			     ntohs(dp->nh.iph->tot_len) +
			     sizeof(struct in_addr), IPPROTO_DSR,
			     dp->nh.iph->ttl);
#endif
		/* Forward RREQ */
		action = DSR_PKT_FORWARD_RREQ;
	}
      out:
	FREE(srt_rev);
	return action;
}

#ifdef __KERNEL__

static int
rreq_tbl_proc_info(char *buffer, char **start, off_t offset, int length)
{
	int len;

	len = rreq_tbl_print(&rreq_tbl, buffer);

	*start = buffer + offset;
	len -= offset;
	if (len > length)
		len = length;
	else if (len < 0)
		len = 0;
	return len;
}

#endif				/* __KERNEL__ */

int __init NSCLASS rreq_tbl_init(void)
{
	INIT_TBL(&rreq_tbl, RREQ_TBL_MAX_LEN);

#ifdef __KERNEL__
	proc_net_create(RREQ_TBL_PROC_NAME, 0, rreq_tbl_proc_info);
	get_random_bytes(&rreq_seqno, sizeof(unsigned int));
#else
	rreq_seqno = 0;
#endif
	return 0;
}

void __exit NSCLASS rreq_tbl_cleanup(void)
{
	struct rreq_tbl_entry *e;

	while ((e = (struct rreq_tbl_entry *)tbl_detach_first(&rreq_tbl))) {
		del_timer_sync(e->timer);
#ifdef NS2
		delete e->timer;
#else
		FREE(e->timer);
#endif
		tbl_flush(&e->rreq_id_tbl, crit_none);
	}
#ifdef __KERNEL__
	proc_net_remove(RREQ_TBL_PROC_NAME);
#endif
}
