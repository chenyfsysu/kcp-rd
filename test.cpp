//=====================================================================
//
// test.cpp - kcp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>

#include "test.h"
#include "ikcp.cpp"


// 模拟网络
LatencySimulator *vnet;

const int SIM_COUNT = 10;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

// 测试用例
void test(int plr, int rtt_min, int rtt_max, int mode, int &out_sumrtt, int &out_maxrtt, int &out_tx)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(plr, rtt_min, rtt_max);

	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
	ikcpcb *kcp1 = async::ikcp_create(0x11223344, (void*)0);
	ikcpcb *kcp2 = async::ikcp_create(0x11223344, (void*)1);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
	kcp1->output = udp_output;
	kcp2->output = udp_output;

	IUINT32 current = iclock();
	IUINT32 slap = current + 20;
	IUINT32 index = 0;
	IUINT32 next = 0;
	IINT64 sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
	async::ikcp_wndsize(kcp1, 128, 128);
	async::ikcp_wndsize(kcp2, 128, 128);

	// 判断测试用例的模式
	if (mode == 0) {
		// 默认模式
		async::ikcp_nodelay(kcp1, 0, 10, 0, 0);
		async::ikcp_nodelay(kcp2, 0, 10, 0, 0);
	}
	else if (mode == 1) {
		// 普通模式，关闭流控等
		async::ikcp_nodelay(kcp1, 0, 10, 0, 1);
		async::ikcp_nodelay(kcp2, 0, 10, 0, 1);
    }
    else if(mode == 2) {
		// 启动快速模式
		// 第二个参数 nodelay-启用以后若干常规加速将启动
		// 第三个参数 interval为内部处理时钟，默认设置为 10ms
		// 第四个参数 resend为快速重传指标，设置为2
		// 第五个参数 为是否禁用常规流控，这里禁止
		async::ikcp_nodelay(kcp1, 1, 10, 2, 1);
		async::ikcp_nodelay(kcp2, 1, 10, 2, 1);
		kcp1->rx_minrto = 10;
		kcp1->fastresend = 1;
	}
    else {
		async::ikcp_nodelay(kcp1, 1, 10, 2, 1);
		async::ikcp_nodelay(kcp2, 1, 10, 2, 1);
		kcp1->rx_minrto = 10;
		kcp1->fastresend = 1;
        async::ikcp_rdcnt(kcp1, 1);
        async::ikcp_rdcnt(kcp2, 1);
    }


	char buffer[2000];
	int hr;

	IUINT32 ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		async::ikcp_update(kcp1, iclock());
		async::ikcp_update(kcp2, iclock());

		// 每隔 20ms，kcp1发送数据
		for (; current >= slap; slap += 20) {
			((IUINT32*)buffer)[0] = index++;
			((IUINT32*)buffer)[1] = current;

			// 发送上层协议包
			async::ikcp_send(kcp1, buffer, 8);
		}

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(1, buffer, 2000);
			if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			async::ikcp_input(kcp2, buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
			hr = vnet->recv(0, buffer, 2000);
			if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
			async::ikcp_input(kcp1, buffer, hr);
		}

		// kcp2接收到任何包都返回回去
		while (1) {
			hr = async::ikcp_recv(kcp2, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			async::ikcp_send(kcp2, buffer, hr);
		}

		// kcp1收到kcp2的回射数据
		while (1) {
			hr = async::ikcp_recv(kcp1, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT32 rtt = current - ts;
			
			if (sn != next) {
				// 如果收到的包不连续
				printf("ERROR sn %d<->%d\n", (int)count, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

			//printf("[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
		}
		if (next > 1000) break;
	}

	ts1 = iclock() - ts1;

	async::ikcp_release(kcp1);
	async::ikcp_release(kcp2);

	const char *names[4] = { "default", "normal", "fast", "redundancy" };
	//printf("%s mode result (%dms):\n", names[mode], (int)ts1);
	//printf("avgrtt=%d maxrtt=%d tx=%d\n", (int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1);
    //printf("f_resnd_times=%d t_resnd_times=%d, rd_snd_times=%d\n", kcp1->f_resnd_times, kcp1->t_resnd_times, kcp1->rd_snd_times);
	//printf("press enter to next ...\n");
	//char ch; scanf("%c", &ch);
    //
    
    out_sumrtt += sumrtt;
    if(maxrtt > out_maxrtt) out_maxrtt = maxrtt;
    out_tx += (int)vnet->tx1;
}


void test_group(int plr, int rtt_min, int rtt_max) {
    printf("**延迟%d-%dms，丢包率%d%%**\n", rtt_min, rtt_max, plr);
    int sumrtt1=0, maxrtt1=0, tx1=0;
    int count = SIM_COUNT;
    while(count--) {
       test(plr, rtt_min, rtt_max, 2, sumrtt1, maxrtt1, tx1); 
    }
    //printf("avgrtt: %d, maxrtt: %d, tx: %d\n", (int)sumrtt1 / (SIM_COUNT * 1000), maxrtt1, (int)tx1 / SIM_COUNT);

    int sumrtt2=0, maxrtt2=0, tx2=0;
    count = SIM_COUNT;
    while(count--) {
       test(plr, rtt_min, rtt_max, 3, sumrtt2, maxrtt2, tx2); 
    }
    //printf("avgrtt: %d, maxrtt: %d, tx: %d\n", (int)sumrtt2 / (SIM_COUNT * 1000), maxrtt2, (int)tx2 / SIM_COUNT);

    int avgrtt1 = (int)sumrtt1 / (SIM_COUNT * 1000);
    int avgrtt2 = (int)sumrtt2 / (SIM_COUNT * 1000);
    int avgtx1 = (int)tx1 / SIM_COUNT;
    int avgtx2 = (int)tx2 / SIM_COUNT; 
    printf("| | 平均rtt | 最大rtt | 发包量\n");
    printf("| ------ | ------ | ------ | ------ |\n");
    printf("| 无冗余 | %d | %d | %d |\n", avgrtt1, maxrtt1, avgtx1);
    printf("| 冗余 | %d | %d | %d |\n", avgrtt2, maxrtt2, avgtx2);
    printf("| 数据对比 | -%.2f%% | -%.2f%% | %.2f%% |\n", (avgrtt1 - avgrtt2) * 100.0 / avgrtt1, (maxrtt1 - maxrtt2) * 100.0 / maxrtt1, (avgtx2 - avgtx1) * 100.0 / avgtx1);
}
int main()
{
    test_group(10, 5,  50);
    test_group(10, 50, 100);
    test_group(10, 100, 150);
    test_group(10, 150, 200);
    test_group(20, 150, 200);
    test_group(20, 200, 300);
    test_group(30, 200, 300);
	return 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/

