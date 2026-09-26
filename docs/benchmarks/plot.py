"""Regenerate the Chinese and English README figures: python3 docs/benchmarks/plot.py.

Requires matplotlib. Input is the adjacent, archived per-run JSON, not local logs.
Chinese output requires Noto Sans CJK SC (installed or supplied with --cjk-font).
SVG embeds glyph paths, so readers do not need the font installed.
"""
import argparse
import json
from pathlib import Path
from statistics import mean
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib import font_manager

args = argparse.ArgumentParser(description=__doc__)
args.add_argument("--cjk-font", type=Path, help="Path to a Chinese font, e.g. NotoSansCJKsc-Regular.otf")
options = args.parse_args()
if options.cjk_font:
    font_manager.fontManager.addfont(options.cjk_font)
    chinese_font = font_manager.FontProperties(fname=options.cjk_font).get_name()
else:
    chinese_font = "Noto Sans CJK SC"
    font_manager.findfont(chinese_font, fallback_to_default=False)

HERE = Path(__file__).resolve().parent
RUNS = json.loads((HERE / '2026-09-26.json').read_text())['runs']
COLORS = {'snowtg': '#167d9a', 'dperf': '#e18b36', 'wrk': '#7d8998'}
plt.rcParams.update({'font.size': 11, 'svg.fonttype': 'path', 'axes.spines.top': False,
                     'axes.spines.right': False, 'axes.titleweight': 'bold', 'svg.hashsalt': 'snowtg-2026-09-26'})


ZH = {
    'Saturation throughput in the measured environment':
        '当前测评环境下的饱和吞吐',
    'Throughput under a client-worker CPU time limit':
        '压测端工作线程受 CPU 时间预算限制时的吞吐',
    '2026-09-26 | NUC i5-1240P / I225-V -> HP Ryzen 7 7730U / RTL8153 | 1 Gbps Ethernet':
        '2026-09-26｜发流端 NUC i5-1240P / I225-V → 服务端 HP Ryzen 7 7730U / RTL8153｜千兆以太网',
    'HTTP/1.1 GET 93 B; response body 3 B; nginx 16 workers; keep-alive enabled':
        'HTTP/1.1 GET 请求 93 字节；响应体 3 字节；nginx 16 个工作进程；启用连接复用',
    'SnowTG c2b9586 + ack3 fixes (quota: aligned3) | dperf 69998e5 | wrk a211dd5 | DPDK 26.07-rc3':
        'SnowTG c2b9586 + ack3 修复（配额组：aligned3）｜dperf 69998e5｜wrk a211dd5｜DPDK 26.07-rc3',
    'Server-received requests (kRPS)':
        '服务端收到的请求（千次/秒）',
    'Client -> server traffic (kPPS)':
        '客户端 → 服务端流量（千包/秒）',
    '1 worker / thread':
        '1 个工作线程',
    '2 workers / threads':
        '2 个工作线程',
    '10% CPU budget':
        '10% CPU 时间预算',
    '20% CPU budget':
        '20% CPU 时间预算',
    '40% CPU budget':
        '40% CPU 时间预算',
    '512 connections; 30 s configured duration; steady window: traffic onset +12 to +27 s.':
        '并发 512；配置时长 30 秒；稳态窗口：开始发流后的第 12～27 秒。',
    'Workers CPU 0 / 0,2 (~4.4 GHz); DPDK main CPU 8; 2 GiB per DPDK tool. dperf adds 10 s slow-start.':
        '工作线程绑定 CPU 0 / 0、2（约 4.4 GHz）；DPDK 主线程绑定 CPU 8；各用 2 GiB；dperf 另有 10 秒慢启动。',
    'SnowTG default RX/TX descriptors: 1024; dperf: 4096. wrk kernel/IRQ work can use additional CPUs.':
        'RX/TX 描述符：SnowTG 默认 1024，dperf 为 4096；wrk 的内核及中断处理可能占用额外 CPU。',
    'All displayed runs: native errors = 0; dperf RST/retransmissions = 0. Wide ranges overlap; no winner established.':
        '图示各轮原生错误均为 0，dperf 重置/重传均为 0；波动范围重叠，不能据均值判断胜负。',
    'Observed end-to-end saturation, NOT an isolated generator CPU limit. Tools ran in separate blocks.':
        '这是本环境的端到端饱和吞吐，尚未隔离发流端 CPU 极限；不同工具分批运行。',
    '1 worker on CPU 0 at 400 MHz; main CPU 8 unrestricted; 2048 connections; 2 GiB per tool.':
        '单工作线程绑定 CPU 0，频率 400 MHz；主线程 CPU 8 不限配额；并发 2048；各用 2 GiB。',
    'Both RX/TX descriptor counts = 4096 (SnowTG temporary build). CFS quota/period: 1/10, 1/5, 1/2.5 ms.':
        '双方 RX/TX 描述符均为 4096（SnowTG 临时构建）；CFS 配额/周期：1/10、1/5、1/2.5 毫秒。',
    '45 s configured duration; attach quota after 10 s warm-up; measure +5 to +20 s after attachment.':
        '配置时长 45 秒；预热 10 秒后施加配额；统计配额生效后第 5～20 秒。',
    'Full-run errors, 3-run sums (10% / 20% / 40%): SnowTG fail 878 / 0 / 0; dperf HTTP+socket 93 / 47 / 0.':
        '三轮全程错误合计（10% / 20% / 40%）：SnowTG 失败 878 / 0 / 0；dperf HTTP/socket 错误 93 / 47 / 0。',
    'dperf RST 35,062 / 59,432 / 53,210. Scheduling stalls and recovery affect results; NOT pure instruction cost.':
        'dperf 重置（RST）35,062 / 59,432 / 53,210；调度暂停与恢复会影响成绩，不能视作纯指令成本。',
    'Mean of 3 runs; whiskers = observed min-max (not a confidence interval). Linear axes start at zero.\nRPS: nginx common-window request counter. PPS: HP NIC RX, includes retransmissions/background traffic.\nSource: docs/benchmarks/2026-09-26.json | methodology, versions and errors: docs/BENCHMARK.md':
        '柱高为三轮均值；误差线为实测最小值～最大值（非置信区间）；纵轴为从零开始的线性刻度。\nRPS：nginx 共同窗口请求计数；PPS：HP 网卡接收计数，包含重传与背景流量。\n数据：docs/benchmarks/2026-09-26.json｜方法、版本与错误：docs/BENCHMARK.md',
    'Three-run means and ranges. ':
        '三轮均值与范围。 ',
}

def plot_language(filename, title, groups, tools, notes, language):
    tr = (lambda text: ZH[text]) if language == "zh" else (lambda text: text)
    title = tr(title)
    groups = [(tr(tick), workers, profile) for tick, workers, profile in groups]
    notes = [tr(note) for note in notes]
    fig, axes = plt.subplots(1, 2, figsize=(14, 7.3))
    fig.subplots_adjust(top=.75, bottom=.34, left=.07, right=.98, wspace=.19)
    fig.suptitle(title, x=.07, y=.96, ha='left', fontsize=20, weight='bold')
    fig.text(.07, .90, tr('2026-09-26 | NUC i5-1240P / I225-V -> HP Ryzen 7 7730U / RTL8153 | 1 Gbps Ethernet'), fontsize=11)
    fig.text(.07, .86, tr('HTTP/1.1 GET 93 B; response body 3 B; nginx 16 workers; keep-alive enabled'), fontsize=11)
    fig.text(.07, .817, tr('SnowTG c2b9586 + ack3 fixes (quota: aligned3) | dperf 69998e5 | wrk a211dd5 | DPDK 26.07-rc3'), fontsize=10, color='#455468')
    width = .23 if len(tools) == 3 else .30
    for ax, metric, label in zip(axes, ('server_rps', 'server_rx_pps'),
                                 ('Server-received requests (kRPS)', 'Client -> server traffic (kPPS)')):
        for j, tool in enumerate(tools):
            for i, (tick, workers, profile) in enumerate(groups):
                values = [r[metric] / 1000 for r in RUNS if r['tool'] == tool and
                          r['mode'] == 'ka' and r['workers'] == workers and r['profile'] == profile]
                assert len(values) == 3, (tool, tick, len(values))
                avg = mean(values)
                x = i + (j - (len(tools)-1)/2)*width
                ax.bar(x, avg, width*.9, color=COLORS[tool], label=tool if i == 0 else None,
                       yerr=[[avg-min(values)], [max(values)-avg]], capsize=4,
                       error_kw={'elinewidth': 1, 'ecolor': '#344050'})
                ax.annotate(f'{avg:.1f}', (x, max(values)), xytext=(0, 6),
                            textcoords='offset points', ha='center', fontsize=10, weight='bold')
        ax.set_title(tr(label), loc='left', pad=16)
        ax.set_xticks(range(len(groups)), [g[0] for g in groups])
        ax.set_ylim(0, ax.get_ylim()[1]*1.15)
        ax.grid(axis='y', alpha=.18)
        ax.set_axisbelow(True)
    axes[0].legend(loc='upper left', frameon=False, ncol=len(tools), bbox_to_anchor=(0, 1.02))
    fig.text(.07, .255, '\n'.join(notes), va='top', fontsize=10, linespacing=1.65)
    fig.text(.07, .025, tr('Mean of 3 runs; whiskers = observed min-max (not a confidence interval). Linear axes start at zero.\n'
             'RPS: nginx common-window request counter. PPS: HP NIC RX, includes retransmissions/background traffic.\n'
             'Source: docs/benchmarks/2026-09-26.json | methodology, versions and errors: docs/BENCHMARK.md'),
             fontsize=9, color='#455468', linespacing=1.5)
    out = HERE.parent / 'assets' / filename.replace('.svg', f'-{language}.svg')
    fig.savefig(out, metadata={'Date': '2026-09-26', 'Title': title,
                              'Description': tr('Three-run means and ranges. ') + ' '.join(notes)})
    # Matplotlib leaves trailing spaces in multiline SVG path attributes.
    out.write_text("\n".join(line.rstrip() for line in out.read_text().splitlines()) + "\n")
    plt.close(fig)


def plot(*args):
    for language, font in (('zh', chinese_font), ('en', 'DejaVu Sans')):
        with plt.rc_context({'font.family': font}):
            plot_language(*args, language=language)


plot('benchmark-capacity.svg', 'Saturation throughput in the measured environment',
     [('1 worker / thread', 1, 'ack'), ('2 workers / threads', 2, 'ack')],
     ['snowtg', 'dperf', 'wrk'], [
         '512 connections; 30 s configured duration; steady window: traffic onset +12 to +27 s.',
         'Workers CPU 0 / 0,2 (~4.4 GHz); DPDK main CPU 8; 2 GiB per DPDK tool. dperf adds 10 s slow-start.',
         'SnowTG default RX/TX descriptors: 1024; dperf: 4096. wrk kernel/IRQ work can use additional CPUs.',
         'All displayed runs: native errors = 0; dperf RST/retransmissions = 0. Wide ranges overlap; no winner established.',
         'Observed end-to-end saturation, NOT an isolated generator CPU limit. Tools ran in separate blocks.'
     ])
plot('benchmark-cpu-budget.svg', 'Throughput under a client-worker CPU time limit',
     [('10% CPU budget', 1, 'quota10-warm-aligned'), ('20% CPU budget', 1, 'quota20-warm-aligned'),
      ('40% CPU budget', 1, 'quota40-warm-aligned')], ['snowtg', 'dperf'], [
         '1 worker on CPU 0 at 400 MHz; main CPU 8 unrestricted; 2048 connections; 2 GiB per tool.',
         'Both RX/TX descriptor counts = 4096 (SnowTG temporary build). CFS quota/period: 1/10, 1/5, 1/2.5 ms.',
         '45 s configured duration; attach quota after 10 s warm-up; measure +5 to +20 s after attachment.',
         'Full-run errors, 3-run sums (10% / 20% / 40%): SnowTG fail 878 / 0 / 0; dperf HTTP+socket 93 / 47 / 0.',
         'dperf RST 35,062 / 59,432 / 53,210. Scheduling stalls and recovery affect results; NOT pure instruction cost.'
     ])
