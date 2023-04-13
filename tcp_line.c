#include <linux/mm.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <net/tcp.h>

#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int alpha __read_mostly = 2;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int gamma __read_mostly = 2;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly;

module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(alpha, int, 0644);
MODULE_PARM_DESC(alpha, "beta for multiplicative increase");
module_param(gamma, int, 0644);
MODULE_PARM_DESC(gamma, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");

struct linetcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	K;
	u32	delay_min;	/* min delay (msec << 3) */
	u32	epoch_start;	/* beginning of an epoch */
};

static inline void linetcp_reset(struct linetcp *ca)
{
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->K = 0;
	ca->delay_min = 0;
	ca->epoch_start = 0;
}

static void linetcp_init(struct sock *sk)
{
	struct linetcp *ca = inet_csk_ca(sk);

	linetcp_reset(ca);

	if (initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;
}

static inline void linetcp_update(struct linetcp *ca, u32 cwnd, u32 acked)
{
	u64 offs, t;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_jiffies32;
		ca->K = ca->last_max_cwnd / alpha;
	}

	t = msecs_to_jiffies(ca->delay_min >> 3);

	if (t < ca->K)
		offs = alpha * t;
	else
		offs = (gamma) * t;

	offs = offs?:100*cwnd;

	ca->cnt = cwnd / offs;
	printk("cwnd:%llu  t:%llu  off:%llu cnt:%llu\n", cwnd, t, offs, ca->cnt);

	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;

	ca->cnt = max(ca->cnt, 2U);
}

static void linetcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct linetcp *ca = inet_csk_ca(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	linetcp_update(ca, tp->snd_cwnd, acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

static u32 linetcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct linetcp *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;	/* end of epoch */
	ca->last_max_cwnd = tp->snd_cwnd;

	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static void linetcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		linetcp_reset(inet_csk_ca(sk));
	}
}

static void linetcp_acked(struct sock *sk, const struct ack_sample *sample)
{
	struct linetcp *ca = inet_csk_ca(sk);
	u32 delay;

	/* Some calls are for duplicates without timetamps */
	if (sample->rtt_us < 0)
		return;

	/* Discard delay samples right after fast recovery */
	if (ca->epoch_start && (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	delay = (sample->rtt_us << 3) / USEC_PER_MSEC;
	if (delay == 0)
		delay = 1;

	/* first time call or link delay decreases */
	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;
}

static struct tcp_congestion_ops culinetcp __read_mostly = {
	.init		= linetcp_init,
	.ssthresh	= linetcp_recalc_ssthresh,
	.cong_avoid	= linetcp_cong_avoid,
	.set_state	= linetcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.pkts_acked     = linetcp_acked,
	.owner		= THIS_MODULE,
	.name		= "line",
};

static int __init culinetcp_register(void)
{
	BUILD_BUG_ON(sizeof(struct linetcp) > ICSK_CA_PRIV_SIZE);

	return tcp_register_congestion_control(&culinetcp);
}

static void __exit culinetcp_unregister(void)
{
	tcp_unregister_congestion_control(&culinetcp);
}

module_init(culinetcp_register);
module_exit(culinetcp_unregister);

MODULE_AUTHOR("Sangtae Ha, Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CUBIC TCP");
MODULE_VERSION("2.3");
