#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/cpumask.h>
#include <linux/string.h>
#include <linux/vmstat.h>
#include <linux/mmzone.h>

#define KP_MAX 512

struct kp_sample {
	u64 t;
	u64 idle_l, idle_m, idle_p;
	u64 iowait, softirq;
	u64 memfree, memavail, swap;
	u64 z_orig, z_compr, z_used;
	u64 s_ref, s_alias, s_delta, s_meta, s_refcap, s_deltacap;
};

static struct kp_sample kp_ring[KP_MAX];
static unsigned int kp_n;
static unsigned int kp_want = 480;
static unsigned int kp_interval = 60;
static bool kp_running;
static struct delayed_work kp_work;
static struct proc_dir_entry *kp_dir;

#ifdef CONFIG_ZRAM
extern int zram_kds_stats(u64 *mm, u64 *sd);
#else
static int zram_kds_stats(u64 *mm, u64 *sd)
{
	return -ENODEV;
}
#endif

static u64 cluster_idle(int lo, int hi)
{
	u64 t = 0;
	int c;

	for (c = lo; c <= hi; c++)
		t += kcpustat_cpu(c).cpustat[CPUTIME_IDLE];
	return t;
}

static u64 total_iowait(void)
{
	u64 t = 0;
	int c;

	for_each_possible_cpu(c)
		t += kcpustat_cpu(c).cpustat[CPUTIME_IOWAIT];
	return t;
}

static u64 total_softirq(void)
{
	u64 t = 0;
	int i, c;

	for_each_possible_cpu(c)
		for (i = 0; i < NR_SOFTIRQS; i++)
			t += kstat_softirqs_cpu(i, c);
	return t;
}

static void kp_collect(struct kp_sample *s)
{
	u64 mm[3] = {0, 0, 0};
	u64 sd[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

	memset(s, 0, sizeof(*s));
	s->t = ktime_get_ns();
	s->idle_l = cluster_idle(0, 3);
	s->idle_m = cluster_idle(4, 6);
	s->idle_p = cluster_idle(7, 7);
	s->iowait = total_iowait();
	s->softirq = total_softirq();

	s->memfree = (u64)global_zone_page_state(NR_FREE_PAGES) << (PAGE_SHIFT - 10);
	s->memavail = (u64)si_mem_available() << (PAGE_SHIFT - 10);
	s->swap = (u64)(total_swap_pages - atomic_long_read(&nr_swap_pages)) << (PAGE_SHIFT - 10);

	if (!zram_kds_stats(mm, sd)) {
		s->z_orig = mm[0] >> 10;
		s->z_compr = mm[1] >> 10;
		s->z_used = mm[2] >> 10;
		s->s_ref = sd[3];
		s->s_alias = sd[4];
		s->s_delta = sd[5];
		s->s_meta = sd[2];
		s->s_refcap = sd[6];
		s->s_deltacap = sd[7];
	}
}

static unsigned int pct_x100(u64 d, u64 base)
{
	if (!base)
		return 0;
	return (unsigned int)((d * 10000ULL) / base);
}

static int kp_report_show(struct seq_file *m, void *v)
{
	unsigned int i;
	u64 t0 = kp_n ? kp_ring[0].t : 0;
	u64 ncpu = num_possible_cpus();

	seq_printf(m, "t_s,idle_l,idle_m,idle_p,iowait,memfree_kb,memavail_kb,swap_kb,z_orig_kb,z_compr_kb,z_used_kb,ref,alias,delta,meta,refcap,deltacap,softirq\n");
	for (i = 0; i < kp_n; i++) {
		struct kp_sample *s = &kp_ring[i];
		struct kp_sample *p = i ? &kp_ring[i - 1] : s;
		u64 dt = s->t - p->t;
		u64 base = ncpu * dt;

		seq_printf(m, "%llu,%u.%02u,%u.%02u,%u.%02u,%u.%02u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
			   (s->t - t0) / 1000000000ULL,
			   pct_x100(s->idle_l - p->idle_l, base) / 100,
			   pct_x100(s->idle_l - p->idle_l, base) % 100,
			   pct_x100(s->idle_m - p->idle_m, base) / 100,
			   pct_x100(s->idle_m - p->idle_m, base) % 100,
			   pct_x100(s->idle_p - p->idle_p, base) / 100,
			   pct_x100(s->idle_p - p->idle_p, base) % 100,
			   pct_x100(s->iowait - p->iowait, base) / 100,
			   pct_x100(s->iowait - p->iowait, base) % 100,
			   s->memfree, s->memavail, s->swap,
			   s->z_orig, s->z_compr, s->z_used,
			   s->s_ref, s->s_alias, s->s_delta, s->s_meta,
			   s->s_refcap, s->s_deltacap,
			   s->softirq - p->softirq);
	}
	return 0;
}

static int kp_snap_show(struct seq_file *m, void *v)
{
	struct kp_sample s;

	kp_collect(&s);
	seq_printf(m, "uptime_s=%llu\n", ktime_get_ns() / 1000000000ULL);
	seq_printf(m, "idle_l_s=%llu\n", s.idle_l / 1000000000ULL);
	seq_printf(m, "idle_m_s=%llu\n", s.idle_m / 1000000000ULL);
	seq_printf(m, "idle_p_s=%llu\n", s.idle_p / 1000000000ULL);
	seq_printf(m, "iowait_ms=%llu\n", s.iowait / 1000000ULL);
	seq_printf(m, "softirq_total=%llu\n", s.softirq);
	seq_printf(m, "memfree_kb=%llu\n", s.memfree);
	seq_printf(m, "memavail_kb=%llu\n", s.memavail);
	seq_printf(m, "swap_used_kb=%llu\n", s.swap);
	seq_printf(m, "zram_orig_kb=%llu\n", s.z_orig);
	seq_printf(m, "zram_compr_kb=%llu\n", s.z_compr);
	seq_printf(m, "zram_used_kb=%llu\n", s.z_used);
	seq_printf(m, "sddc_ref=%llu\n", s.s_ref);
	seq_printf(m, "sddc_alias=%llu\n", s.s_alias);
	seq_printf(m, "sddc_delta=%llu\n", s.s_delta);
	seq_printf(m, "sddc_meta=%llu\n", s.s_meta);
	seq_printf(m, "sddc_refcap=%llu\n", s.s_refcap);
	seq_printf(m, "sddc_deltacap=%llu\n", s.s_deltacap);
	seq_printf(m, "samples=%u\n", kp_n);
	seq_printf(m, "running=%d\n", kp_running ? 1 : 0);
	seq_printf(m, "interval_s=%u\n", kp_interval);
	seq_printf(m, "want=%u\n", kp_want);
	return 0;
}

static void kp_workfn(struct work_struct *w)
{
	if (!kp_running)
		return;
	if (kp_n >= kp_want) {
		kp_running = false;
		return;
	}

	kp_collect(&kp_ring[kp_n]);
	kp_n++;

	if (kp_n < kp_want)
		schedule_delayed_work(&kp_work, kp_interval * HZ);
	else
		kp_running = false;
}

static ssize_t kp_ctl_write(struct file *f, const char __user *ub,
			    size_t n, loff_t *ppos)
{
	char b[48];
	int iv = 0, cnt = 0;

	if (n >= sizeof(b))
		return -EINVAL;
	if (copy_from_user(b, ub, n))
		return -EFAULT;
	b[n] = 0;

	if (!strncmp(b, "start", 5)) {
		if (sscanf(b + 5, "%d %d", &iv, &cnt) < 1)
			return -EINVAL;
		if (iv < 1)
			iv = 1;
		if (iv > 3600)
			iv = 3600;
		if (cnt < 1)
			cnt = KP_MAX;
		if (cnt > KP_MAX)
			cnt = KP_MAX;
		kp_interval = iv;
		kp_want = cnt;
		kp_n = 0;
		kp_running = true;
		schedule_delayed_work(&kp_work, msecs_to_jiffies(1000));
	} else if (!strncmp(b, "stop", 4)) {
		kp_running = false;
		cancel_delayed_work_sync(&kp_work);
	} else if (!strncmp(b, "clear", 5)) {
		kp_n = 0;
	} else {
		return -EINVAL;
	}

	return n;
}

static const struct proc_ops kp_ctl_fops = {
	.proc_write = kp_ctl_write,
};

static int __init kdsprobe_init(void)
{
	INIT_DELAYED_WORK(&kp_work, kp_workfn);

	kp_dir = proc_mkdir("kdsprobe", NULL);
	if (!kp_dir)
		return -ENOMEM;

	proc_create("ctl", 0222, kp_dir, &kp_ctl_fops);
	proc_create_single("snap", 0444, kp_dir, kp_snap_show);
	proc_create_single("report", 0444, kp_dir, kp_report_show);

	pr_info("kdsprobe: ready at /proc/kdsprobe\n");
	return 0;
}
module_init(kdsprobe_init);

static void __exit kdsprobe_exit(void)
{
	kp_running = false;
	cancel_delayed_work_sync(&kp_work);
	remove_proc_subtree("kdsprobe", NULL);
}
module_exit(kdsprobe_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kds companion probe");
