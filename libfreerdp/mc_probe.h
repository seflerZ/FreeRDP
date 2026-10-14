/**
 * FreeRDP - 临时打点开关(本 fork 排查用,非上游文件)
 *
 * 2026-10-14 排查 xrdp 拖窗口卡顿留下的埋点。默认【关】。
 *
 * 要复现那轮排查,把下面的 MC_PROBE 改成 1 重新编译 —— 只需要改这一处,
 * 用到打点的三个文件(codec/h264.c、codec/h264_mediacodec.c、gdi/gfx.c)
 * 都从这里取。
 *
 *   关:计时退化成常量 0,日志退化成空语句,优化器会把整块统计算术清掉,
 *       热路径上不剩什么开销。
 *   开:每 50 帧往 <app>/files/mctime.log 追加一行,单位统一微秒。
 *       先删掉旧文件再看,否则新旧两轮的 n= 计数会串在一起。
 *
 * 日志行格式(开的时候):
 *   AVC420 n=… period=…us decomp=…us yuv=…us own=…us    整帧视角
 *   MCDEC  n=… drain=…us feed=…us drainmax=…us queued=… 硬解内部 + 流水线深度
 *   GFX    n=… blit=…us paint=…us px=…K rects=…        surface→primary 拷贝 + JNI
 *   YUV    n=… wall=…us cpu=…us ratio=… generic=0/1     YUV→RGB 的墙钟/CPU 时间
 *
 * 重要判据(当轮踩过的):
 *   period ≈ decomp+yuv+blit+paint ⇒ 客户端算力受限
 *   period 明显更大              ⇒ 在等服务端出帧(服务端是天花板)
 *   YUV ratio 明显 > 1           ⇒ 被调度出去/抢内存带宽,不是算得慢
 */

#ifndef FREERDP_MC_PROBE_H
#define FREERDP_MC_PROBE_H

#define MC_PROBE 0

#if MC_PROBE

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#define MC_MARK(v) ((v) = mc_now_us())
#define MC_MARK_CPU(v) ((v) = mc_now_cpu_us())

/* 单调墙钟,微秒 */
static inline double mc_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* 本线程真正吃掉的 CPU(不含被调度出去的墙钟时间)。和 mc_now_us 一比就能分出
   "算得慢" 还是 "被抢占/抢带宽"。 */
static inline double mc_now_cpu_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static inline void mc_log(const char* fmt, ...)
{
	FILE* f;
	va_list ap;
	static const char* paths[2] = {"/data/data/com.qihua.rmt/files/mctime.log",
	                               "/sdcard/Android/data/com.qihua.rmt/files/mctime.log"};
	int i;

	(void)mkdir("/data/data/com.qihua.rmt/files", 0770);
	(void)mkdir("/sdcard/Android/data/com.qihua.rmt/files", 0770);

	for (i = 0; i < 2; i++)
	{
		f = fopen(paths[i], "a");

		if (!f)
			continue;

		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fclose(f);
		return;
	}
}

#else /* MC_PROBE */

/*
 * 关掉时这几样都化成空语句。调用点的写法配合同一套宏:
 *
 *     #if MC_PROBE
 *         double t0, t1;          // 计时变量只在开的时候才声明
 *     #endif
 *         MC_MARK(t0);            // 关的时候整行消失,不会引用不存在的变量
 *         do_the_real_work();
 *         MC_MARK(t1);
 *     #if MC_PROBE
 *         ...统计累加 + mc_log(...)...   // 关的时候整块不存在,不留残留代码
 *     #endif
 *
 * 这样关掉之后热路径上不多一条指令,也不会冒 -Wunused-but-set-variable。
 */
#define MC_MARK(v) ((void)0)
#define MC_MARK_CPU(v) ((void)0)

#endif /* MC_PROBE */

#endif /* FREERDP_MC_PROBE_H */
