"""
osl_reader.py の watch をリアルタイムのグラフで表示する。

osl_reader.py --watch SEC --plot から呼ばれる。読み出し・コンソール表示・CSV 書き出しは
osl_reader.Watcher がそのまま行い、ここでは描画だけを受け持つ。

グラフの作り:
  - 子機ごとに 1 行、計測値ごとに 1 列の小さなグラフを並べる。単位の違う値
    (気圧と温度など) を 1 枚に重ねると縦軸が 2 本になって読み違えるので、必ず分ける。
  - 1 枚に 1 系列なので凡例は付けず、タイトルに子機と計測値の名前、右上に最新値を出す。
  - 状態が正常でない値 (status ≠ 0) と読み出し失敗は線を途切れさせて示す。
    最後の値を引き延ばすと、止まっているのに動いているように見えてしまう。
  - マウスを重ねると、その時刻の値を表示する。拡大・移動はツールバーで行う。
  - 横軸は直近 span 秒だけを表示する。データは全期間 CSV に残っている。

必要なもの: pip install matplotlib
"""
import os
import time
import datetime
from collections import deque

import matplotlib
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.ticker import FormatStrFormatter, MaxNLocator

from osl_reader import VALUE_DECIMALS

# ===== 色と線 =============================================================
SURFACE   = '#fcfcfb'   # グラフ面
PAGE      = '#f9f9f7'   # ウィンドウ背景
INK       = '#0b0b0b'   # 主要な文字 (最新値)
INK_2     = '#52514e'   # タイトル
MUTED     = '#898781'   # 軸ラベル・目盛
GRID      = '#e1e0d9'   # 目盛線
AXIS      = '#c3c2b7'   # 軸線
SERIES    = '#2a78d6'   # 系列 (どのグラフも 1 系列なので同じ色)
LINE_W    = 2.0

# 日本語のタイトルを出すため、Windows に入っているゴシック体を使う
matplotlib.rcParams['font.family'] = ['Yu Gothic', 'Meiryo', 'MS Gothic', 'DejaVu Sans']
matplotlib.rcParams['axes.unicode_minus'] = False

def value_labels(info: dict) -> tuple:
    """計測値の名前。v2 ではチャネル記述にチャネル名が入っている"""
    return tuple(n or f'ch{i}' for i, n in enumerate(info['ch_names']))


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    for side in ('top', 'right', 'left'):
        ax.spines[side].set_visible(False)
    ax.spines['bottom'].set_color(AXIS)
    ax.grid(True, axis='y', color=GRID, linewidth=0.8)
    ax.tick_params(colors=MUTED, labelsize=8, length=0)
    # 目盛も最新値と同じ桁数で出す。気圧 (9.3e4 Pa 前後) でも「+9.303e4」のような
    # オフセット表記にならない
    ax.yaxis.set_major_formatter(FormatStrFormatter(f'%.{VALUE_DECIMALS}f'))
    ax.yaxis.set_major_locator(MaxNLocator(nbins=6, steps=[1, 2, 5, 10]))
    ax.xaxis.set_major_locator(mdates.AutoDateLocator(minticks=3, maxticks=6))
    ax.xaxis.set_major_formatter(mdates.DateFormatter('%H:%M:%S'))


class Panel:
    """1 子機 × 1 計測値のグラフ"""

    def __init__(self, ax, addr: int, index: int, maxlen: int):
        self.ax = ax
        self.addr = addr
        self.index = index
        self.t = deque(maxlen=maxlen)
        self.v = deque(maxlen=maxlen)
        style_axes(ax)
        (self.line,) = ax.plot([], [], color=SERIES, linewidth=LINE_W,
                               solid_capstyle='round', solid_joinstyle='round')
        self.title = ax.set_title('', loc='left', fontsize=9, color=INK_2)
        self.latest = ax.text(1.0, 1.02, '', transform=ax.transAxes, ha='right', va='bottom',
                              fontsize=12, color=INK, fontweight='bold')
        # ホバー時の縦線と値表示
        self.cursor = ax.axvline(0, color=MUTED, linewidth=0.8, visible=False)
        self.marker = ax.plot([], [], 'o', markersize=8, color=SERIES,
                              markeredgecolor=SURFACE, markeredgewidth=2, visible=False)[0]
        self.tip = ax.annotate('', xy=(0, 0), xytext=(8, 8), textcoords='offset points',
                               fontsize=8, color=INK, visible=False,
                               bbox=dict(boxstyle='round,pad=0.4', fc='white', ec=GRID))
        self.unit = ''

    def append(self, t, info, poll):
        i = self.index
        if poll is None or poll['status'][i] != 0:
            value = float('nan')        # 線を途切れさせる
        else:
            value = poll['values'][i]
        self.t.append(t)
        self.v.append(value)

        if info is not None:
            self.unit = info['unit_names'][i]
            label = value_labels(info)[i]
            self.title.set_text(f'0x{self.addr:02X} {info["model_name"]} — {label} [{self.unit}]')
        else:
            # 開始時に応答しなかった子機は機種も単位も分からない
            self.title.set_text(f'0x{self.addr:02X} 応答なし')
        if value == value:              # NaN でなければ
            self.latest.set_text(f'{value:.{VALUE_DECIMALS}f} {self.unit}')
            self.latest.set_color(INK)
        else:
            self.latest.set_text('無効' if poll is not None else '読み出し失敗')
            self.latest.set_color(MUTED)

    def redraw(self, span_sec: float):
        if not self.t:
            return
        self.line.set_data(list(self.t), list(self.v))
        t_end = self.t[-1]
        t_start = t_end - datetime.timedelta(seconds=span_sec)
        self.ax.set_xlim(t_start, t_end)
        # 表示範囲の値だけで縦軸を合わせる (外れた古い値に引っ張られない)
        shown = [v for t, v in zip(self.t, self.v) if t >= t_start and v == v]
        if shown:
            lo, hi = min(shown), max(shown)
            # 目盛は小数点以下 VALUE_DECIMALS 桁で出すので、表示幅がその刻みの 5 倍より
            # 狭いと同じ数字の目盛が並ぶ。幅に下限を設ける。
            min_span = 5 * 10 ** -VALUE_DECIMALS
            pad = max((hi - lo) * 0.1, (min_span - (hi - lo)) / 2)
            self.ax.set_ylim(lo - pad, hi + pad)

    def hover(self, x):
        """マウス位置 x (matplotlib の日付数値) に最も近い点の値を表示する"""
        if not self.t:
            return False
        times = mdates.date2num(list(self.t))
        k = min(range(len(times)), key=lambda j: abs(times[j] - x))
        t, v = self.t[k], self.v[k]
        self.cursor.set_xdata([t, t])
        self.cursor.set_visible(True)
        stamp = t.strftime('%H:%M:%S')
        if v == v:
            self.marker.set_data([t], [v])
            self.marker.set_visible(True)
            self.tip.xy = (mdates.date2num(t), v)
            self.tip.set_text(f'{stamp}\n{v:.{VALUE_DECIMALS}f} {self.unit}')
        else:
            self.marker.set_visible(False)
            ymid = sum(self.ax.get_ylim()) / 2
            self.tip.xy = (mdates.date2num(t), ymid)
            self.tip.set_text(f'{stamp}\n値なし')
        # 右端付近では吹き出しを左側に出して、グラフからはみ出さないようにする
        x0, x1 = self.ax.get_xlim()
        self.tip.set_position((-80, 8) if x > x0 + (x1 - x0) * 0.7 else (8, 8))
        self.tip.set_visible(True)
        return True

    def unhover(self):
        changed = self.cursor.get_visible()
        for artist in (self.cursor, self.marker, self.tip):
            artist.set_visible(False)
        return changed


def run(watcher, interval: float, span_sec: float):
    """グラフのウィンドウを開き、閉じるまで interval 秒ごとに読み出して描き足す"""
    addrs = watcher.addrs
    n_cols = max((i['count'] for i in watcher.infos.values() if i), default=1)
    n_rows = len(addrs)
    maxlen = int(span_sec / interval) + 2

    fig, axes = plt.subplots(n_rows, n_cols, squeeze=False,
                             figsize=(5.5 * n_cols, 2.6 * n_rows + 0.6))
    fig.patch.set_facecolor(PAGE)
    fig.canvas.manager.set_window_title(f'OSL 子機 — CSV: {os.path.splitext(watcher.csv_path)[0]}_0xNN.csv')

    panels = []
    for r, a in enumerate(addrs):
        info = watcher.infos[a]
        count = info['count'] if info else 1
        for c in range(n_cols):
            ax = axes[r][c]
            if c < count:
                panels.append(Panel(ax, a, c, maxlen))
            else:
                ax.set_visible(False)

    # 開始時刻からの一定間隔の格子で読む (watch と同じ理由)。タイマーは細かく回し、
    # 予定時刻を過ぎたときだけ読み出す。
    state = {'next': time.perf_counter()}

    def on_timer():
        if time.perf_counter() < state['next']:
            return
        state['next'] += interval
        now = datetime.datetime.now()
        by_addr = {a: (info, poll) for a, info, poll in watcher.tick()}
        for p in panels:
            info, poll = by_addr[p.addr]
            p.append(now, info, poll)
            p.redraw(span_sec)
        fig.canvas.draw_idle()

    def on_move(event):
        changed = False
        for p in panels:
            if event.inaxes is p.ax and event.xdata is not None:
                changed |= p.hover(event.xdata)
            else:
                changed |= p.unhover()
        if changed:
            fig.canvas.draw_idle()

    fig.canvas.mpl_connect('motion_notify_event', on_move)
    timer = fig.canvas.new_timer(interval=50)
    timer.add_callback(on_timer)
    timer.start()
    on_timer()
    # タイトルと最新値の文字が入ってから余白を決める (空のうちに決めると上が切れる)
    fig.tight_layout(h_pad=2.0, w_pad=2.0)

    print('グラフのウィンドウを閉じると終了します')
    plt.show()
    timer.stop()
