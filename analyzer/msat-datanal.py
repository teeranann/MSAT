"""
MSAT Data Analyzer  (V.Y2026.88.209)
============================================================================
Desktop analysis app for Multi-Sensor Automatic Titrator (MSAT) data files.
Loads a run (.txt), derives pH/EC/colour-dE/temperature curves + derivatives,
detects equivalence points (1st/2nd derivative + inflection trendlines),
cleans glitches (hiccup despike), and can stream a live run from the device
WebSocket. Three-tab config (Data / Analysis / Display) + results rail.

Copyright (c) 2026 Burapha University. All rights reserved.
Inventor / developer: Teeranan Nongnual <teeranan.no@buu.ac.th>
  Department of Chemistry, Faculty of Science, Burapha University, Thailand
Petty Patent pending: Application No. 2603001145 (filed 2026-05-12).

License: PolyForm Noncommercial 1.0.0 (see /LICENSE) - free for noncommercial
use (education/research/schools/universities), modification allowed, selling
prohibited.
SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0
Required Notice: Copyright (c) 2026 Burapha University.

Run:   python msat-datanal.py
Deps:  see requirements.txt  (pip install -r requirements.txt)
"""

import sys
import os
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.ticker import FormatStrFormatter, MultipleLocator
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog
import ttkbootstrap as ttk
from ttkbootstrap.constants import *
from scipy.signal import savgol_filter, find_peaks
from skimage import color
import ctypes
from ctypes import wintypes
import io
from PIL import Image
import requests
import threading
import time
import json
try:
    import websocket  # websocket-client: listen to the device /ws telemetry
    _HAS_WS = True
except Exception:
    _HAS_WS = False

try:
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
except Exception:
    pass

try:
    from tkinterdnd2 import TkinterDnD, DND_FILES
    _HAS_DND = True
except ImportError:
    _HAS_DND = False


# ==========================================
# 1. ALGORITHMS — VERBATIM FROM ORIGINAL
# ==========================================
class Algo:
    @staticmethod
    def smooth_savgol(y, window, poly=2):
        if len(y) < 5: return y
        if window < 3: window = 3
        if window % 2 == 0: window += 1
        if len(y) < window: window = len(y) // 2 * 2 + 1
        if window < 3: return y
        try:
            return savgol_filter(y, window, poly)
        except:
            return y

    @staticmethod
    def hampel(y, win=7, n_sig=3.0):
        # Robust isolated-spike remover (rolling median + MAD). Replaces only
        # points that deviate > n_sig robust-sigmas from their local median,
        # so single-sample glitches (e.g. the EC drop-out spike) are cleaned
        # while a real titration inflection - a sustained step over many
        # points - is left untouched (edge-preserving).
        y = np.asarray(y, dtype=float)
        n = len(y)
        if n < 5:
            return y
        if win < 3: win = 3
        if win % 2 == 0: win += 1
        half = win // 2
        out = y.copy()
        k = 1.4826  # MAD -> sigma for normal data
        for i in range(n):
            a = max(0, i - half)
            b = min(n, i + half + 1)
            w = y[a:b]
            med = np.median(w)
            mad = k * np.median(np.abs(w - med))
            if mad > 0 and np.abs(y[i] - med) > n_sig * mad:
                out[i] = med
        return out

    @staticmethod
    def find_peaks_1st(x, y, prom_factor, height_factor, dist_factor, target_peaks):
        try:
            max_y = np.max(y) if len(y) > 0 else 0
            if max_y == 0: return []
            abs_prom = max_y * prom_factor
            abs_height = max_y * height_factor
            if len(x) > 1:
                avg_dx = (x[-1] - x[0]) / len(x)
                if avg_dx <= 0: avg_dx = 1.0
            else:
                avg_dx = 1.0
            total_vol = x[-1] if len(x) > 0 and x[-1] > 0 else 100.0
            interval_vol = total_vol / max(1, target_peaks)
            dist_vol = interval_vol * dist_factor
            min_dist_indices = int(dist_vol / avg_dx)
            if min_dist_indices < 1: min_dist_indices = 1
            peaks, props = find_peaks(y, prominence=abs_prom, height=abs_height, distance=min_dist_indices)
            if len(peaks) > 0:
                sorted_indices = np.argsort(props['prominences'])[::-1]
                return peaks[sorted_indices]
            return []
        except:
            return []

    @staticmethod
    def find_intersection(x, y, guess_x, span_pct, skip_pct, poly_order=1, min_slope_diff=0.005):
        try:
            idx = (np.abs(x - guess_x)).argmin()
            n = len(x)
            gap_pts = max(2, int(n * (skip_pct / 100.0)))
            span_pts = max(4, int(n * (span_pct / 100.0)))
            l_end = max(0, idx - gap_pts)
            l_start = max(0, l_end - span_pts)
            x_L = x[l_start:l_end]; y_L = y[l_start:l_end]
            r_start = min(n, idx + gap_pts)
            r_end = min(n, r_start + span_pts)
            x_R = x[r_start:r_end]; y_R = y[r_start:r_end]
            if len(x_L) < poly_order + 1 or len(x_R) < poly_order + 1: return None
            pL = np.polyfit(x_L, y_L, poly_order)
            pR = np.polyfit(x_R, y_R, poly_order)
            if poly_order == 1:
                slope_L = pL[0]; slope_R = pR[0]
                if abs(slope_L - slope_R) < min_slope_diff: return None
            diff = np.subtract(pL, pR)
            roots = np.roots(diff)
            real_roots = roots[np.isreal(roots)].real
            total_range = x.max() - x.min()
            valid_roots = [r for r in real_roots if abs(r - guess_x) < total_range * 0.2]
            if valid_roots:
                root = min(valid_roots, key=lambda r: abs(r - guess_x))
                return (root, (pL, x_L), (pR, x_R))
            return None
        except:
            return None


# ==========================================
# 2. CLIPBOARD HELPER — VERBATIM
# ==========================================
class Clipboard:
    @staticmethod
    def to_clipboard(image):
        GMEM_MOVEABLE = 0x0002
        CF_DIB = 8
        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        kernel32.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
        kernel32.GlobalAlloc.restype = ctypes.c_void_p
        kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
        kernel32.GlobalLock.restype = ctypes.c_void_p
        kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
        user32.SetClipboardData.argtypes = [wintypes.UINT, ctypes.c_void_p]
        try:
            output = io.BytesIO()
            image.convert("RGB").save(output, "BMP")
            data = output.getvalue()[14:]
            output.close()
            hcd = kernel32.GlobalAlloc(GMEM_MOVEABLE, len(data))
            if not hcd: raise Exception("GlobalAlloc failed")
            pch = kernel32.GlobalLock(hcd)
            if not pch: raise Exception("GlobalLock failed")
            ctypes.memmove(pch, data, len(data))
            kernel32.GlobalUnlock(hcd)
            if not user32.OpenClipboard(None): raise Exception("OpenClipboard failed")
            user32.EmptyClipboard()
            if not user32.SetClipboardData(CF_DIB, hcd): raise Exception("SetClipboardData failed")
            user32.CloseClipboard()
        except Exception as e:
            try: user32.CloseClipboard()
            except: pass
            raise e


# ==========================================
# 3. MAIN APP — NEW LAYOUT
# ==========================================
class MSAT_Redesign(ttk.Window):
    # Palette (matches the HTML redesign)
    C_BG       = "#eef0f3"
    C_PANEL    = "#fafbfc"
    C_CARD     = "#ffffff"
    C_BORDER   = "#d8dde4"
    C_TEXT     = "#0f172a"
    C_MUTED    = "#64748b"
    C_PRIMARY  = "#1d4ed8"
    C_ORANGE   = "#f97316"
    C_RED      = "#dc2626"
    C_GREEN    = "#10b981"
    C_HEADBG   = "#1d4ed8"

    def __init__(self):
        super().__init__(themename="cosmo")
        if _HAS_DND:
            try:
                TkinterDnD._require(self)
            except Exception:
                pass
        self.title("MSAT Data Analyzer V.Y2026.88.209 — Redesigned")
        self.geometry("2560x1520")
        self.minsize(1280, 760)
        self.configure(background=self.C_BG)

        # ---- Data state (identical to original) ----
        self.df_raw = None
        self.df_cal = None
        self.source_type = "OFFLINE"
        self.source_path = ""
        self.is_monitoring = False
        self.monitor_thread = None
        self.last_line_count = 0
        self.stop_event = threading.Event()

        # ---- Live Monitor (poll device /data while a titration runs) ----
        self.msat_ip = "192.168.1.200"
        self.live_monitor_active = False
        self.live_thread = None
        self.live_rows = []
        self.msat_status_var = tk.StringVar(value="MSAT: not checked")

        self.file_label_var = tk.StringVar(value="No file loaded")
        self.table_view_mode = tk.StringVar(value="first1000")
        self.xaxis_var = tk.StringVar(value="mL")
        self.density_var = tk.DoubleVar(value=1.0)
        self.conc_var = tk.DoubleVar(value=0.1)
        self.drop_rate_var = tk.StringVar(value="Drop rate = N/A")
        self.xmax_var = tk.StringVar(value="80.00")
        self.de_mode = tk.StringVar(value="CIELAB")

        self.algo = {
            "sm_ph": tk.IntVar(value=15), "sm_col": tk.IntVar(value=15),
            "sm_ec": tk.IntVar(value=15), "sm_temp": tk.IntVar(value=100),
            "sm_deriv": tk.IntVar(value=15),
            "prom_factor": tk.DoubleVar(value=0.20),
            "ht_factor": tk.DoubleVar(value=0.10),
            "dist_factor": tk.DoubleVar(value=0.40)
        }
        self.peak_sets = {
            "num_peak": tk.IntVar(value=1),
            "main_sensor": tk.StringVar(value="pH"),
            "manual_guess": tk.StringVar(value="0.0"),
            "search_win": tk.DoubleVar(value=15.0),
            "method_ph": tk.IntVar(value=1), "method_col": tk.IntVar(value=1),
            "method_ec": tk.IntVar(value=3), "method_temp": tk.IntVar(value=3),
            "int_span": tk.DoubleVar(value=15.0),
            "int_skip": tk.DoubleVar(value=2.0),
            "int_poly": tk.IntVar(value=1),
            "slope_diff": tk.DoubleVar(value=0.005)
        }
        self.show_trendlines = tk.BooleanVar(value=False)
        self.temp_last_only = tk.BooleanVar(value=True)
        self.show_dt_in_graph = tk.BooleanVar(value=True)
        self.dt_mode = tk.StringVar(value="ep_min")

        self.phi_var = tk.DoubleVar(value=4.01)
        self.phf_var = tk.DoubleVar(value=9.18)
        self.ph_autoscale_active = False

        self.vis = {"pt_size": tk.DoubleVar(value=0.2), "font": tk.DoubleVar(value=12)}
        self.yaxis = {
            "pH_min": tk.StringVar(value="-0.50"), "pH_max": tk.StringVar(value="14.50"), "pH_auto": tk.BooleanVar(value=False),
            "dE_min": tk.StringVar(value=""), "dE_max": tk.StringVar(value=""), "dE_auto": tk.BooleanVar(value=True),
            "EC_min": tk.StringVar(value=""), "EC_max": tk.StringVar(value=""), "EC_auto": tk.BooleanVar(value=True),
            "T_min":  tk.StringVar(value=""), "T_max":  tk.StringVar(value=""), "T_auto":  tk.BooleanVar(value=True),
        }
        self.ph_fixed_mode = tk.BooleanVar(value=True)
        self.yaxis_digits = {
            "pH": tk.IntVar(value=0), "dE": tk.IntVar(value=1),
            "EC": tk.IntVar(value=0), "T":  tk.IntVar(value=1),
        }
        # Y-axis major tick spacing per sensor (blank = auto). pH default 7
        # gives ticks at 0, 7, 14.
        self.yaxis_interval = {
            "pH": tk.StringVar(value="7"), "dE": tk.StringVar(value=""),
            "EC": tk.StringVar(value=""), "T":  tk.StringVar(value=""),
        }
        self.ec_unit_var = tk.StringVar(value="uS/cm")
        self.ec_unit_auto = tk.BooleanVar(value=True)

        self.found_eps = {}
        self.trendlines_data = {}
        self.global_anchors = []
        self.ep_visibility = {}
        self.ep_visibility_applied = {}
        self.analysis_done = False
        # ---- Data Processing state ----
        # Auto clean-hiccup removes single-sample X-axis (Weightloss) glitches
        # like a value that jumps then reverts; manual_excluded holds original
        # df_raw index labels removed by clicking points on the dashboard.
        self.clean_hiccup_on = tk.BooleanVar(value=True)
        self.hiccup_nsig = tk.DoubleVar(value=4.0)
        self.manual_excluded = set()
        self.df_raw_clean = None
        self._pre_max_geometry = None
        self._btn_maximize = None
        self._is_maximized = False
        self.fig = Figure(figsize=(10, 8), dpi=100)

        self._setup_styles()
        self._build_ui()
        self.after(1000, self.update_status_led)

    # ============================================================
    # STYLE
    # ============================================================
    def _setup_styles(self):
        s = ttk.Style()
        s.configure("AppBar.TFrame", background=self.C_HEADBG)
        s.configure("AppBar.TLabel", background=self.C_HEADBG, foreground="#ffffff",
                    font=("Helvetica", 13, "bold"))
        s.configure("AppBarVer.TLabel", background=self.C_HEADBG, foreground="#cbd5e1",
                    font=("Consolas", 9))
        s.configure("Source.TLabel", background="#1e40af", foreground="#ffffff",
                    font=("Consolas", 9), padding=(8, 4))
        s.configure("DropRate.TLabel", background=self.C_HEADBG, foreground="#e0e7ff",
                    font=("Consolas", 9))

        s.configure("Card.TLabelframe", background=self.C_CARD, borderwidth=1, relief="solid")
        s.configure("Card.TLabelframe.Label", background=self.C_CARD,
                    foreground=self.C_TEXT, font=("Helvetica", 9, "bold"))

        s.configure("ConfigTabs.TNotebook", background=self.C_PANEL, borderwidth=0)
        s.configure("ConfigTabs.TNotebook.Tab", padding=(18, 8),
                    font=("Helvetica", 10, "bold"))
        s.map("ConfigTabs.TNotebook.Tab",
              background=[("selected", "#ffffff"), ("!selected", "#f1f5f9")],
              foreground=[("selected", self.C_PRIMARY), ("!selected", self.C_MUTED)])

        s.configure("Dash.TNotebook", background="#ffffff", borderwidth=0)
        s.configure("Dash.TNotebook.Tab", padding=(20, 9), font=("Helvetica", 10, "bold"))
        s.map("Dash.TNotebook.Tab",
              background=[("selected", "#ffffff"), ("!selected", "#f8fafc")],
              foreground=[("selected", self.C_PRIMARY), ("!selected", self.C_MUTED)])

        s.configure("Field.TLabel", background=self.C_CARD, foreground="#475569",
                    font=("Helvetica", 9, "bold"))
        s.configure("Hint.TLabel", background=self.C_CARD, foreground="#94a3b8",
                    font=("Helvetica", 8, "italic"))

    # ============================================================
    # LAYOUT
    # ============================================================
    def _build_ui(self):
        self._build_appbar()

        body = tk.Frame(self, bg=self.C_BG)
        body.pack(fill=BOTH, expand=True)

        # 3-column grid: config | dashboard | results
        body.columnconfigure(0, minsize=350, weight=0)
        body.columnconfigure(1, weight=1)
        body.columnconfigure(2, minsize=320, weight=0)
        body.rowconfigure(0, weight=1)

        self._build_config_panel(body)
        self._build_dashboard(body)
        self._build_results_panel(body)

    # ---------- App bar ----------
    def _build_appbar(self):
        bar = ttk.Frame(self, style="AppBar.TFrame")
        bar.pack(fill=X)

        ttk.Label(bar, text="⚛", style="AppBar.TLabel",
                  font=("Helvetica", 16, "bold")).pack(side=LEFT, padx=(14, 6), pady=8)
        ttk.Label(bar, text="MSAT Data Analyzer", style="AppBar.TLabel").pack(side=LEFT)
        ttk.Label(bar, text=" v.Y2026.88.208", style="AppBarVer.TLabel").pack(side=LEFT, padx=(4, 14))

        # Source pill (LED + label)
        src_holder = tk.Frame(bar, bg="#1e40af")
        src_holder.pack(side=LEFT, padx=(0, 10), pady=8)
        self.led_canvas = tk.Canvas(src_holder, width=12, height=12, highlightthickness=0, bg="#1e40af")
        self.led_id = self.led_canvas.create_oval(1, 1, 11, 11, fill="gray", outline="")
        self.led_canvas.pack(side=LEFT, padx=(8, 6), pady=4)
        ttk.Label(src_holder, textvariable=self.file_label_var, style="Source.TLabel").pack(side=LEFT)

        # Right side
        ttk.Button(bar, text="✕  Close", bootstyle="danger",
                   command=self.on_close).pack(side=RIGHT, padx=(0, 14), pady=8)
        self._btn_maximize = ttk.Button(bar, text="⛶  Full / Restore", bootstyle="secondary-outline",
                                        command=self._toggle_maximize)
        self._btn_maximize.pack(side=RIGHT, padx=(0, 4), pady=8)
        ttk.Label(bar, textvariable=self.drop_rate_var,
                  style="DropRate.TLabel").pack(side=RIGHT, padx=10)

    # ---------- Config panel (left) ----------
    def _build_config_panel(self, parent):
        panel = tk.Frame(parent, bg=self.C_PANEL,
                         highlightbackground=self.C_BORDER, highlightthickness=0)
        panel.grid(row=0, column=0, sticky="nsew")
        self.config_panel = panel  # for live-monitor UI lock

        nb = ttk.Notebook(panel, style="ConfigTabs.TNotebook")
        nb.pack(fill=BOTH, expand=True, padx=0, pady=0)

        # Each tab: scrollable area on top, sticky footer with primary action below
        data_inner, data_footer = self._tab_with_footer(nb, "📡  Data")
        analysis_inner, analysis_footer = self._tab_with_footer(nb, "⚙  Analysis")
        display_inner, display_footer = self._tab_with_footer(nb, "🎨  Display")

        self._build_data_tab(data_inner)
        self._build_analysis_tab(analysis_inner)
        self._build_display_tab(display_inner)

        ttk.Button(analysis_footer, text="⚡  Run Analysis", bootstyle="warning",
                   command=self.run_analysis).pack(fill=X, padx=10, pady=10)
        ttk.Button(display_footer, text="🔄  Refresh Graph", bootstyle="primary",
                   command=self.update_graph_only).pack(fill=X, padx=10, pady=10)

    def _tab_with_footer(self, notebook, text):
        """Return (scrollable_inner_frame, footer_frame) for a notebook tab."""
        wrap = tk.Frame(notebook, bg=self.C_PANEL)
        notebook.add(wrap, text=text)

        # Footer first (packed bottom so it always reserves its space)
        footer = tk.Frame(wrap, bg="#ffffff",
                          highlightbackground="#e2e8f0", highlightthickness=1)
        footer.pack(side=BOTTOM, fill=X)

        # Scrollable content above
        cf = tk.Frame(wrap, bg=self.C_PANEL)
        cf.pack(side=TOP, fill=BOTH, expand=True)
        canvas = tk.Canvas(cf, bg=self.C_PANEL, highlightthickness=0, borderwidth=0)
        vsb = ttk.Scrollbar(cf, orient="vertical", command=canvas.yview)
        inner = tk.Frame(canvas, bg=self.C_PANEL)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        window_id = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.bind("<Configure>", lambda e: canvas.itemconfig(window_id, width=e.width))
        canvas.configure(yscrollcommand=vsb.set)
        canvas.pack(side=LEFT, fill=BOTH, expand=True)
        vsb.pack(side=RIGHT, fill=Y)
        canvas.bind("<Enter>", lambda e: canvas.bind_all("<MouseWheel>",
                    lambda ev: canvas.yview_scroll(int(-1*(ev.delta/120)), "units")))
        canvas.bind("<Leave>", lambda e: canvas.unbind_all("<MouseWheel>"))
        return inner, footer

    def _card(self, parent, title, subtitle=None):
        outer = tk.Frame(parent, bg=self.C_PANEL)
        outer.pack(fill=X, padx=10, pady=6)
        card = tk.Frame(outer, bg=self.C_CARD,
                        highlightbackground="#e2e8f0", highlightthickness=1)
        card.pack(fill=X)
        head = tk.Frame(card, bg="#f8fafc")
        head.pack(fill=X)
        tk.Label(head, text=title.upper(), bg="#f8fafc", fg=self.C_TEXT,
                 font=("Helvetica", 9, "bold")).pack(side=LEFT, padx=10, pady=6)
        if subtitle:
            tk.Label(head, text=subtitle, bg="#f8fafc", fg="#94a3b8",
                     font=("Helvetica", 8, "italic")).pack(side=RIGHT, padx=10, pady=6)
        body = tk.Frame(card, bg=self.C_CARD)
        body.pack(fill=X, padx=10, pady=10)
        return body

    def _label(self, parent, text, bold=False):
        return tk.Label(parent, text=text, bg=self.C_CARD, fg="#475569",
                        font=("Helvetica", 9, "bold" if bold else "normal"))

    # ----- Data tab -----
    def _build_data_tab(self, p):
        # Source card
        b = self._card(p, "Source")
        self._build_drop_zone(b)
        row = tk.Frame(b, bg=self.C_CARD); row.pack(fill=X)
        self.btn_local_file = ttk.Button(row, text="📂  Local File", bootstyle="warning",
                   command=self.load_local_file, width=16)
        self.btn_local_file.pack(side=LEFT, padx=(0, 6))
        tk.Label(b, textvariable=self.file_label_var, bg=self.C_CARD, fg="#64748b",
                 font=("Consolas", 8), wraplength=320, justify="left", anchor="w").pack(fill=X, pady=(8, 0))

        # Live Monitor: check device status, then stream /data while running
        live = tk.Frame(b, bg=self.C_CARD); live.pack(fill=X, pady=(8, 0))
        self.btn_check = ttk.Button(live, text="📡  Check MSAT", bootstyle="info",
                   command=self.check_msat_status, width=14)
        self.btn_check.pack(side=LEFT, padx=(0, 6))
        self.btn_live = ttk.Button(live, text="▶ Live Monitor", bootstyle="success",
                                   command=self.toggle_live_monitor, width=15, state="disabled")
        self.btn_live.pack(side=LEFT)
        tk.Label(b, textvariable=self.msat_status_var, bg=self.C_CARD, fg="#475569",
                 font=("Consolas", 8), anchor="w").pack(fill=X, pady=(4, 0))

        # Axis & Scaling
        b = self._card(p, "Axis & Scaling")
        row = tk.Frame(b, bg=self.C_CARD); row.pack(fill=X)
        self._label(row, "X-Axis", bold=True).pack(side=LEFT, padx=(0, 8))
        for v, t in [("mL", "mL"), ("mol", "mol"), ("sec", "sec"), ("g", "g")]:
            ttk.Radiobutton(row, text=t, variable=self.xaxis_var, value=v,
                            command=self.update_axis_inputs).pack(side=LEFT, padx=2)

        grid = tk.Frame(b, bg=self.C_CARD); grid.pack(fill=X, pady=(8, 0))
        self._label(grid, "Density (g/cm³)").grid(row=0, column=0, sticky="w", padx=(0, 8), pady=3)
        self.ent_density = ttk.Entry(grid, textvariable=self.density_var, width=10)
        self.ent_density.grid(row=0, column=1, sticky="w", pady=3)
        self._label(grid, "Concentration (M)").grid(row=1, column=0, sticky="w", padx=(0, 8), pady=3)
        self.ent_conc = ttk.Entry(grid, textvariable=self.conc_var, width=10)
        self.ent_conc.grid(row=1, column=1, sticky="w", pady=3)
        ttk.Button(grid, text="Apply", bootstyle="primary", width=8,
                   command=self.apply_axis_settings).grid(row=0, column=2, rowspan=2, padx=12)

        de_row = tk.Frame(b, bg=self.C_CARD); de_row.pack(fill=X, pady=(8, 0))
        self._label(de_row, "Y-Axis", bold=True).pack(side=LEFT, padx=(0, 8))
        self._label(de_row, "ΔE Mode").pack(side=LEFT, padx=(0, 8))
        ttk.Combobox(de_row, textvariable=self.de_mode, values=["CIELAB", "RGB Eucl."],
                     state="readonly", width=12).pack(side=LEFT)

        # Data Processing (clean-hiccup + manual delete + smoothing)
        b = self._card(p, "Data Processing", subtitle="hiccup clean + Savitzky-Golay")
        rowc = tk.Frame(b, bg=self.C_CARD); rowc.pack(fill=X)
        ttk.Checkbutton(rowc, text="Auto clean hiccup", variable=self.clean_hiccup_on,
                        bootstyle="primary-round-toggle",
                        command=self._recompute_cleaning).pack(side=LEFT)
        self._label(rowc, "Sensitivity (σ)").pack(side=LEFT, padx=(12, 4))
        ttk.Spinbox(rowc, from_=2.0, to=10.0, increment=0.5, width=5,
                    textvariable=self.hiccup_nsig,
                    command=self._recompute_cleaning).pack(side=LEFT)
        rowb = tk.Frame(b, bg=self.C_CARD); rowb.pack(fill=X, pady=(6, 0))
        ttk.Button(rowb, text="🧹 Clean hiccup now", bootstyle="primary", width=18,
                   command=self._recompute_cleaning).pack(side=LEFT, padx=(0, 6))
        ttk.Button(rowb, text="↺ Reset deletes", bootstyle="secondary", width=14,
                   command=self._reset_manual_deletes).pack(side=LEFT)
        tk.Label(b, text="Tip: click a point on any chart to delete that sample.",
                 bg=self.C_CARD, fg="#94a3b8",
                 font=("Helvetica", 8, "italic")).pack(anchor="w", pady=(6, 2))
        self._spin_grid(b, [
            ("Smooth pH", self.algo["sm_ph"], 1, 0, 999),
            ("Smooth Color", self.algo["sm_col"], 1, 0, 999),
            ("Smooth EC", self.algo["sm_ec"], 1, 0, 999),
            ("Smooth Temp", self.algo["sm_temp"], 1, 0, 999),
            ("Smooth Deriv.", self.algo["sm_deriv"], 1, 0, 999),
        ], cols=2)
        rowap = tk.Frame(b, bg=self.C_CARD); rowap.pack(fill=X, pady=(8, 0))
        ttk.Button(rowap, text="✓ Apply smoothing", bootstyle="primary", width=18,
                   command=self._recompute_cleaning).pack(side=LEFT)
        tk.Label(rowap, text="(re-applies smooth + clean)", bg=self.C_CARD,
                 fg="#94a3b8", font=("Helvetica", 8, "italic")).pack(side=LEFT, padx=(8, 0))

        # pH calibration
        b = self._card(p, "pH Calibration")
        row = tk.Frame(b, bg=self.C_CARD); row.pack(fill=X)
        self._label(row, "Initial pH").pack(side=LEFT, padx=(0, 4))
        ttk.Entry(row, textvariable=self.phi_var, width=6).pack(side=LEFT, padx=(0, 12))
        self._label(row, "Final pH").pack(side=LEFT, padx=(0, 4))
        ttk.Entry(row, textvariable=self.phf_var, width=6).pack(side=LEFT, padx=(0, 12))
        self.btn_scale = ttk.Button(row, text="Apply", bootstyle="primary",
                                    width=8, command=self.apply_ph_scale)
        self.btn_scale.pack(side=LEFT)
        tk.Label(b, text="Maps voltage → pH using two-point linear fit.",
                 bg=self.C_CARD, fg="#94a3b8",
                 font=("Helvetica", 8, "italic")).pack(anchor="w", pady=(6, 0))

    # ----- Analysis tab -----
    def _build_analysis_tab(self, p):
        # Filter & Smooth and \u0394E Mode moved to the Data tab ("Data Processing"
        # / "Axis & Scaling"). Analysis tab keeps detection/EP settings.
        b = self._card(p, "Peak Detection", subtitle="global thresholds")
        self._spin_grid(b, [
            ("Peak Count", self.peak_sets["num_peak"], 1, 1, 10),
            ("Window %", self.peak_sets["search_win"], 1, 5, 100),
            ("Prominence %", self.algo["prom_factor"], 0.05, 0.01, 1.0),
            ("Height %", self.algo["ht_factor"], 0.05, 0.01, 1.0),
            ("Distance %", self.algo["dist_factor"], 0.1, 0.1, 5.0),
        ], cols=2)

        b = self._card(p, "Sensor Methods", subtitle="EP-finding strategy")
        row = tk.Frame(b, bg=self.C_CARD); row.pack(fill=X)
        self._label(row, "Main Reference", bold=True).pack(side=LEFT, padx=(0, 6))
        for s_ in ["pH", "Color", "EC", "Temp", "Manual"]:
            ttk.Radiobutton(row, text=s_, variable=self.peak_sets["main_sensor"],
                            value=s_).pack(side=LEFT, padx=2)
        row2 = tk.Frame(b, bg=self.C_CARD); row2.pack(fill=X, pady=(4, 0))
        self._label(row2, "Manual guess").pack(side=LEFT, padx=(0, 6))
        ttk.Entry(row2, textvariable=self.peak_sets["manual_guess"], width=10).pack(side=LEFT)

        # Method matrix
        mat = tk.Frame(b, bg=self.C_CARD); mat.pack(fill=X, pady=(10, 4))
        # Header
        for c, t in enumerate(["Sensor", "1st Deriv.", "2nd Deriv.", "Inflection"]):
            tk.Label(mat, text=t, bg=self.C_CARD, fg="#64748b",
                     font=("Helvetica", 8, "bold")).grid(row=0, column=c, padx=8, pady=2, sticky="w" if c == 0 else "")
        for r, (lbl, key) in enumerate([("pH","method_ph"),("Color","method_col"),("EC","method_ec"),("Temperature","method_temp")], start=1):
            tk.Label(mat, text=lbl, bg=self.C_CARD, fg=self.C_TEXT,
                     font=("Helvetica", 9, "bold")).grid(row=r, column=0, sticky="w", padx=8, pady=2)
            for c, v in enumerate([1, 2, 3], start=1):
                ttk.Radiobutton(mat, variable=self.peak_sets[key], value=v).grid(row=r, column=c, pady=2)

        ttk.Checkbutton(b, text="Temperature: Last EP Only", variable=self.temp_last_only,
                        bootstyle="danger-round-toggle").pack(anchor="w", pady=(8, 0))
        ttk.Checkbutton(b, text="Show ΔT in graph", variable=self.show_dt_in_graph,
                        bootstyle="danger-round-toggle",
                        command=self.update_graph_only).pack(anchor="w", pady=(4, 0))
        dt_mode_row = tk.Frame(b, bg=self.C_CARD)
        dt_mode_row.pack(anchor="w", padx=(22, 0), pady=(2, 0))
        ttk.Radiobutton(dt_mode_row, text="Max−Min", variable=self.dt_mode, value="max_min",
                        command=self.update_graph_only).pack(side=LEFT, padx=(0, 10))
        ttk.Radiobutton(dt_mode_row, text="Last EP−Min", variable=self.dt_mode, value="ep_min",
                        command=self.update_graph_only).pack(side=LEFT)

        b = self._card(p, "Inflection Config", subtitle="trendline fit")
        self._spin_grid(b, [
            ("Span %", self.peak_sets["int_span"], 1, 1, 50),
            ("Skip %", self.peak_sets["int_skip"], 1, 0, 20),
            ("Poly Order", self.peak_sets["int_poly"], 1, 1, 3),
            ("Slope Diff", self.peak_sets["slope_diff"], 0.001, 0.001, 0.1),
        ], cols=2)
        ttk.Checkbutton(b, text="Show Trendlines", variable=self.show_trendlines,
                        bootstyle="primary-round-toggle").pack(anchor="w", pady=(8, 0))

    # ----- Display tab -----
    def _build_display_tab(self, p):
        b = self._card(p, "Plot Appearance")
        self._spin_grid(b, [
            ("Point Size", self.vis["pt_size"], 0.1, 0.1, 5),
            ("Font Size", self.vis["font"], 1, 8, 24),
        ], cols=2)
        row = tk.Frame(b, bg=self.C_CARD); row.pack(fill=X, pady=(8, 0))
        self._label(row, "X Maximum").pack(side=LEFT, padx=(0, 8))
        ttk.Entry(row, textvariable=self.xmax_var, width=10).pack(side=LEFT)

        row2 = tk.Frame(b, bg=self.C_CARD); row2.pack(fill=X, pady=(8, 0))
        self._label(row2, "EC Unit").pack(side=LEFT, padx=(0, 8))
        self.cmb_ec_unit = ttk.Combobox(row2, textvariable=self.ec_unit_var,
                                        values=["uS/cm", "mS/cm"], state="readonly", width=10)
        self.cmb_ec_unit.pack(side=LEFT)
        self.cmb_ec_unit.bind("<<ComboboxSelected>>", self.on_ec_unit_changed)
        self.btn_ec_unit_auto = ttk.Button(row2, text="Auto", width=6,
                                           command=lambda: self.set_ec_unit_auto(True))
        self.btn_ec_unit_auto.pack(side=LEFT, padx=(6, 0))

        # Y-Axis ranges grid
        b = self._card(p, "Y-Axis Ranges", subtitle="blank = auto")
        grid = tk.Frame(b, bg=self.C_CARD); grid.pack(fill=X)
        for c, t in enumerate(["Sensor", "Min", "Max", "Digits", "Interval", "Auto", "Fix"]):
            tk.Label(grid, text=t, bg=self.C_CARD, fg="#64748b",
                     font=("Helvetica", 8, "bold")).grid(row=0, column=c, padx=4, pady=2, sticky="w")
        self.axis_buttons = {}
        self.axis_fixed_buttons = {}
        for r, (lbl, key) in enumerate([("pH","pH"),("dE","dE"),("EC","EC"),("Temp","T")], start=1):
            tk.Label(grid, text=lbl, bg=self.C_CARD, fg=self.C_TEXT,
                     font=("Helvetica", 9, "bold")).grid(row=r, column=0, padx=4, sticky="w")
            ent_min = ttk.Entry(grid, textvariable=self.yaxis[f"{key}_min"], width=7)
            ent_min.grid(row=r, column=1, padx=2, pady=1)
            ent_min.bind("<KeyRelease>", lambda e, k=key: self.on_axis_entry_change(k))
            ent_max = ttk.Entry(grid, textvariable=self.yaxis[f"{key}_max"], width=7)
            ent_max.grid(row=r, column=2, padx=2, pady=1)
            ent_max.bind("<KeyRelease>", lambda e, k=key: self.on_axis_entry_change(k))
            ent_dec = ttk.Entry(grid, textvariable=self.yaxis_digits[key], width=3)
            ent_dec.grid(row=r, column=3, padx=2, pady=1)
            ent_dec.bind("<KeyRelease>", lambda e, k=key: self.on_axis_digit_change(k))
            ent_int = ttk.Entry(grid, textvariable=self.yaxis_interval[key], width=6)
            ent_int.grid(row=r, column=4, padx=2, pady=1)
            ent_int.bind("<KeyRelease>", lambda e: self.update_graph_only())
            btn_a = ttk.Button(grid, text="A", width=3, command=lambda k=key: self.set_axis_auto(k, True))
            btn_a.grid(row=r, column=5, padx=2)
            self.axis_buttons[key] = btn_a
            if key == "pH":
                btn_f = ttk.Button(grid, text="F", width=3, command=self.set_ph_fixed)
                btn_f.grid(row=r, column=6, padx=2)
                self.axis_fixed_buttons[key] = btn_f
            else:
                tk.Label(grid, text="—", bg=self.C_CARD, fg="#cbd5e1").grid(row=r, column=6)
        for k in self.axis_buttons.keys():
            self.update_axis_auto_button(k)
        self.update_ph_fixed_button()
        self.update_ec_unit_auto_button()

    def _spin_grid(self, parent, items, cols=2):
        """Layout (label, spinbox) pairs in a grid."""
        wrap = tk.Frame(parent, bg=self.C_CARD); wrap.pack(fill=X)
        for i, item in enumerate(items):
            label, var, inc, frm, to = item
            r, c = divmod(i, cols)
            cell = tk.Frame(wrap, bg=self.C_CARD)
            cell.grid(row=r, column=c, padx=(0, 4) if c == 0 else (0, 0), pady=2, sticky="w")
            tk.Label(cell, text=label, bg=self.C_CARD, fg="#475569",
                     font=("Helvetica", 9, "bold"), width=13, anchor="w").pack(side=LEFT)
            ttk.Spinbox(cell, textvariable=var, from_=frm, to=to,
                        increment=inc, width=7).pack(side=LEFT)

    # ---------- Dashboard (middle) ----------
    def _build_dashboard(self, parent):
        wrap = tk.Frame(parent, bg="#f1f5f9")
        wrap.grid(row=0, column=1, sticky="nsew")

        # Row-mode toggle strip (above the notebook tabs)
        rm = tk.Frame(wrap, bg="#ffffff",
                      highlightbackground=self.C_BORDER, highlightthickness=1)
        rm.pack(fill=X)
        ttk.Radiobutton(rm, text="First 1000 Rows", value="first1000",
                        variable=self.table_view_mode,
                        command=self.on_table_view_mode_changed).pack(side=RIGHT, padx=4, pady=4)
        ttk.Radiobutton(rm, text="Show All Rows", value="all",
                        variable=self.table_view_mode,
                        command=self.on_table_view_mode_changed).pack(side=RIGHT, padx=4, pady=4)

        # Notebook fills the remaining space
        self.notebook = ttk.Notebook(wrap, style="Dash.TNotebook")
        self.notebook.pack(fill=BOTH, expand=True)

        self.tab_dash = ttk.Frame(self.notebook); self.notebook.add(self.tab_dash, text="Dashboard")
        self.tab_cal = ttk.Frame(self.notebook); self.notebook.add(self.tab_cal, text="Calculated Data")
        self.tab_raw = ttk.Frame(self.notebook); self.notebook.add(self.tab_raw, text="Raw Sensor Data")

        # Chart canvas in dashboard tab
        self.embed_fig()

        # Cal / Raw tables
        self.setup_table(self.tab_cal, "cal")
        self.setup_table(self.tab_raw, "raw")

        self.update_axis_inputs()

    # ---------- Results (right) ----------
    def _build_results_panel(self, parent):
        wrap = tk.Frame(parent, bg=self.C_CARD,
                        highlightbackground=self.C_BORDER, highlightthickness=1)
        wrap.grid(row=0, column=2, sticky="nsew")
        self.results_panel = wrap  # for live-monitor UI lock

        head = tk.Frame(wrap, bg=self.C_CARD)
        head.pack(fill=X, padx=12, pady=(12, 6))
        tk.Label(head, text="Results Summary", bg=self.C_CARD, fg=self.C_TEXT,
                 font=("Helvetica", 11, "bold")).pack(anchor="w")
        tk.Label(head, text="Toggle ✓/✗ on EP rows to include/exclude.",
                 bg=self.C_CARD, fg="#64748b",
                 font=("Helvetica", 8, "italic")).pack(anchor="w")

        # Treeview
        tree_wrap = tk.Frame(wrap, bg=self.C_CARD); tree_wrap.pack(fill=BOTH, expand=True, padx=8, pady=6)
        self.tree = ttk.Treeview(tree_wrap, columns=("Show", "Param", "Value"), show="headings", height=18)
        self.tree.heading("Show", text=""); self.tree.column("Show", width=32, anchor="center")
        self.tree.heading("Param", text="Parameter"); self.tree.column("Param", width=140, anchor="w")
        self.tree.heading("Value", text="Value"); self.tree.column("Value", width=80, anchor="e")
        sc = ttk.Scrollbar(tree_wrap, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sc.set)
        self.tree.pack(side=LEFT, fill=BOTH, expand=True)
        sc.pack(side=RIGHT, fill=Y)
        self.tree.bind('<Button-1>', self.on_summary_click)

        # Actions
        actions = tk.Frame(wrap, bg=self.C_CARD)
        actions.pack(fill=X, padx=12, pady=(6, 12))
        ttk.Button(actions, text="Apply EP Selection", bootstyle="primary",
                   command=self.apply_ep_selection).pack(fill=X, pady=(0, 8))

        exp_wrap = tk.Frame(actions, bg=self.C_CARD,
                            highlightbackground="#e2e8f0", highlightthickness=1)
        exp_wrap.pack(fill=X, pady=(4, 0))
        tk.Label(exp_wrap, text="Export Tools", bg=self.C_CARD, fg="#475569",
                 font=("Helvetica", 9, "bold")).pack(anchor="w", padx=8, pady=(6, 2))
        exp = tk.Frame(exp_wrap, bg=self.C_CARD)
        exp.pack(fill=X, padx=6, pady=(0, 6))
        for c in range(2): exp.columnconfigure(c, weight=1)
        ttk.Button(exp, text="📋 Summary", command=self.copy_summary_text,
                   bootstyle="light").grid(row=0, column=0, padx=2, pady=2, sticky="ew")
        ttk.Button(exp, text="📋 Cal Data", command=lambda: self.copy_data("cal"),
                   bootstyle="light").grid(row=0, column=1, padx=2, pady=2, sticky="ew")
        ttk.Button(exp, text="📋 Raw Data", command=lambda: self.copy_data("raw"),
                   bootstyle="light").grid(row=1, column=0, padx=2, pady=2, sticky="ew")
        ttk.Button(exp, text="🖼 Image", command=self.copy_img_clipboard,
                   bootstyle="light").grid(row=1, column=1, padx=2, pady=2, sticky="ew")
        ttk.Button(exp, text="💾 Save Image", command=self.save_image,
                   bootstyle="secondary").grid(row=2, column=0, columnspan=2, padx=2, pady=(4, 2), sticky="ew")

    # ============================================================
    # SHARED HELPERS (unchanged behaviour)
    # ============================================================
    def setup_table(self, parent, tag):
        container = ttk.Frame(parent)
        container.pack(fill=BOTH, expand=True)
        tree = ttk.Treeview(container, columns=["Data"], show="headings")
        tree.grid(row=0, column=0, sticky="nsew")
        sc_y = ttk.Scrollbar(container, orient="vertical", command=tree.yview); sc_y.grid(row=0, column=1, sticky="ns")
        sc_x = ttk.Scrollbar(container, orient="horizontal", command=tree.xview); sc_x.grid(row=1, column=0, sticky="ew")
        tree.configure(yscrollcommand=sc_y.set, xscrollcommand=sc_x.set)
        container.rowconfigure(0, weight=1); container.columnconfigure(0, weight=1)
        if tag == "raw": self.tree_raw = tree
        else: self.tree_cal = tree

    def embed_fig(self):
        self.canvas = FigureCanvasTkAgg(self.fig, master=self.tab_dash)
        self.canvas.draw()
        self.canvas.get_tk_widget().pack(side=TOP, fill=BOTH, expand=True)
        self.canvas.mpl_connect('pick_event', self._on_pick_point)

    def _on_pick_point(self, event):
        # Click a data point on any dashboard plot to delete that sample row.
        art = getattr(event, 'artist', None)
        idxs = getattr(art, '_orig_idx', None)
        if idxs is None or len(event.ind) == 0:
            return
        try:
            orig = int(idxs[int(event.ind[0])])
        except Exception:
            return
        self.manual_excluded.add(orig)
        self.calculate_derived_data(analyze=self.analysis_done)

    def update_axis_inputs(self):
        mode = self.xaxis_var.get()
        if mode == "mL":
            self.ent_density.configure(state="normal"); self.ent_conc.configure(state="disabled")
            self.xmax_var.set("80.00")
        elif mode == "mol":
            self.ent_density.configure(state="normal"); self.ent_conc.configure(state="normal")
            self.xmax_var.set("0.01")
        elif mode == "g":
            self.ent_density.configure(state="disabled"); self.ent_conc.configure(state="disabled")
            self.xmax_var.set("80.00")
        else:
            self.ent_density.configure(state="disabled"); self.ent_conc.configure(state="disabled")
            self.xmax_var.set("600")

    def get_table_view_df(self, df):
        if df is None: return None
        return df if self.table_view_mode.get() == "all" else df.head(1000)

    def on_table_view_mode_changed(self):
        if self.df_raw is not None: self.update_raw_tree_ui()
        if self.df_cal is not None: self.update_cal_tree()

    def on_axis_digit_change(self, key):
        try:
            d = int(str(self.yaxis_digits[key].get()))
        except Exception:
            d = 0
        d = max(0, min(6, d))
        self.yaxis_digits[key].set(d)
        self.update_graph_only()

    def update_ec_unit_auto_button(self):
        try:
            self.btn_ec_unit_auto.configure(bootstyle=("success" if self.ec_unit_auto.get() else "light"))
        except Exception: pass

    def update_axis_auto_button(self, key):
        btn = self.axis_buttons.get(key)
        if not btn: return
        try: is_auto = bool(self.yaxis[f"{key}_auto"].get())
        except Exception: is_auto = False
        if key == "pH" and self.ph_fixed_mode.get(): is_auto = False
        btn.configure(bootstyle=("success" if is_auto else "light"))

    def update_ph_fixed_button(self):
        btn_f = self.axis_fixed_buttons.get("pH")
        if not btn_f: return
        btn_f.configure(bootstyle=("danger" if self.ph_fixed_mode.get() else "light"))

    def _apply_ec_unit_auto_from_df(self, df):
        try:
            if not self.ec_unit_auto.get() or df is None or 'EC_mS' not in df.columns: return
            # Decide by the PEAK EC: once the curve reaches the tens-of-
            # thousands uS range (>= 10 mS) it reads better in mS/cm. Using
            # the max (not min) so runs that start near 0 still switch.
            max_ec_mS = np.nanmax(df['EC_mS'].values) if len(df) > 0 else np.nan
            if np.isfinite(max_ec_mS):
                target_unit = 'mS/cm' if max_ec_mS >= 10.0 else 'uS/cm'
                if self.ec_unit_var.get() != target_unit:
                    self.ec_unit_var.set(target_unit)
        except Exception: pass

    def set_ec_unit_auto(self, on=True):
        self.ec_unit_auto.set(bool(on))
        self.update_ec_unit_auto_button()
        if self.ec_unit_auto.get() and self.df_cal is not None:
            self._apply_ec_unit_auto_from_df(self.df_cal)
        self.update_graph_only()

    def on_ec_unit_changed(self, event=None):
        if self.ec_unit_auto.get():
            self.ec_unit_auto.set(False); self.update_ec_unit_auto_button()
        self.update_graph_only()

    def set_ph_fixed(self, on=None):
        if on is None: on = not self.ph_fixed_mode.get()
        self.ph_fixed_mode.set(bool(on))
        if self.ph_fixed_mode.get():
            self.yaxis['pH_auto'].set(False)
            self.yaxis['pH_min'].set("-0.50"); self.yaxis['pH_max'].set("14.50")
        self.update_axis_auto_button("pH"); self.update_ph_fixed_button()
        self.update_graph_only()

    def _apply_unique_y_formatter(self, ax, base_digits):
        digits = max(0, min(6, base_digits))
        ax.yaxis.set_major_formatter(FormatStrFormatter(f"%.{digits}f"))
        return digits

    def _nice_interval(self, span):
        # Pick a clean tick step (1/2/5 x 10^n) aiming for ~4 intervals.
        # e.g. dE span ~4 -> 1, EC span ~45 -> 10, Temp span ~8 -> 2.
        if not np.isfinite(span) or span <= 0:
            return None
        raw = span / 4.0
        mag = 10.0 ** np.floor(np.log10(raw))
        norm = raw / mag
        if norm < 1.5:   nice = 1.0
        elif norm < 3.0: nice = 2.0
        elif norm < 7.0: nice = 5.0
        else:            nice = 10.0
        return nice * mag

    def apply_axis_settings(self):
        try:
            self.update_axis_inputs()
            self.calculate_derived_data(analyze=False)
            self.refresh_plots()
        except Exception: pass

    # ============================================================
    # LOADING / MONITORING (unchanged)
    # ============================================================
    def load_local_file(self):
        path = filedialog.askopenfilename(filetypes=[("Data", "*.txt *.csv")])
        if path:
            self._load_file_path(path)

    def _build_drop_zone(self, parent):
        """Dashed drag-and-drop zone; falls back to click-to-browse if tkinterdnd2 is absent."""
        self._dz_hovered = False
        c = tk.Canvas(parent, height=120, bg=self.C_CARD, highlightthickness=0, cursor="hand2")
        c.pack(fill=X, pady=(0, 8))
        self._dz = c
        c.bind("<Configure>", lambda e: self._draw_dz())
        c.bind("<Button-1>", lambda e: self.load_local_file())
        c.bind("<Enter>", lambda e: self._dz_set_hover(True))
        c.bind("<Leave>", lambda e: self._dz_set_hover(False))
        if _HAS_DND:
            try:
                c.drop_target_register(DND_FILES)
                c.dnd_bind("<<Drop>>", self._on_dnd_drop)
                c.dnd_bind("<<DragEnter>>", lambda e: (self._dz_set_hover(True), e.action)[1])
                c.dnd_bind("<<DragLeave>>", lambda e: (self._dz_set_hover(False), e.action)[1])
            except Exception:
                pass

    def _draw_dz(self):
        c = self._dz
        c.delete("all")
        w = max(c.winfo_width(), 10)
        h = max(c.winfo_height(), 72)
        bg     = "#eff6ff" if self._dz_hovered else self.C_CARD
        border = "#3b82f6" if self._dz_hovered else "#94a3b8"
        text1  = "#1d4ed8" if self._dz_hovered else "#475569"
        c.configure(bg=bg)
        c.create_rectangle(4, 4, w - 4, h - 4, outline=border, dash=(7, 4), width=2)
        c.create_text(w // 2, h // 2 - 10, text="\U0001f4c4  Drop file here",
                      fill=text1, font=("Helvetica", 10, "bold"))
        c.create_text(w // 2, h // 2 + 10, text="or click to browse",
                      fill="#64748b", font=("Helvetica", 8, "italic"))

    def _dz_set_hover(self, on):
        self._dz_hovered = bool(on)
        self._draw_dz()

    def _on_dnd_drop(self, event):
        import re
        raw = event.data.strip()
        # tkinterdnd2 wraps paths with spaces in {braces}
        matches = re.findall(r'\{([^}]+)\}|(\S+)', raw)
        paths = [a or b for a, b in matches]
        for p in paths:
            p = p.strip()
            if os.path.isfile(p):
                self._load_file_path(p)
                break
        self._dz_set_hover(False)
        return event.action

    def _load_file_path(self, path):
        """Programmatic local-file load — shared by dialog + CLI auto-load."""
        if self.live_monitor_active:
            self.stop_live_monitor()  # loading a file exits live mode + unlocks UI
        self.clear_all_data(); self.stop_monitoring()
        self.source_type = "OFFLINE"; self.source_path = path
        self.file_label_var.set(f"OFFLINE: {os.path.basename(path)}")
        # Auto-restore the saved analysis view if a sidecar exists next to
        # this data file. Apply BEFORE parsing so the first derive uses the
        # saved smoothing/clean/axis settings.
        self._try_load_config_sidecar(path)
        self.last_line_count = 0
        self.is_monitoring = True
        self.start_monitor_thread()

    def clear_all_data(self):
        self.found_eps = {}; self.global_anchors = []; self.trendlines_data = {}
        self.analysis_done = False
        self.drop_rate_var.set("Drop rate = N/A")
        self.tree.delete(*self.tree.get_children())
        self.tree_raw.delete(*self.tree_raw.get_children())
        self.tree_cal.delete(*self.tree_cal.get_children())
        self.df_raw = None; self.df_cal = None
        self.refresh_plots()

    def stop_monitoring(self):
        self.is_monitoring = False; self.stop_event.set()
        if self.monitor_thread and self.monitor_thread.is_alive():
            self.monitor_thread.join(timeout=1.0)
        self.stop_event.clear()

    # ============================================================
    # LIVE MONITOR  - poll the device /data endpoint while running
    # ------------------------------------------------------------
    # Coarse real-time view built from the same telemetry the web
    # dashboard uses (no SD file access, so it never disturbs a run).
    # Accumulated points are fed through the normal derived-data
    # pipeline so the live plot looks like a (lower-resolution) file.
    # ============================================================
    @staticmethod
    def _state_name(s):
        return {0: "IDLE", 1: "PENDING", 2: "RUNNING", 3: "RINSING", 4: "REC ONLY"}.get(s, f"? ({s})")

    def check_msat_status(self):
        self.msat_status_var.set("MSAT: checking...")
        def work():
            try:
                r = requests.get(f"http://{self.msat_ip}/data", timeout=4)
                d = r.json()
                state = int(d.get("state", -1))
                # Device reachable -> allow arming the monitor from ANY state
                # (including IDLE). The monitor waits and captures the next run.
                self.after(0, lambda: self._apply_status(self._state_name(state), True))
            except Exception:
                self.after(0, lambda: self._apply_status("OFFLINE", False))
        threading.Thread(target=work, daemon=True).start()

    def _apply_status(self, name, online):
        if self.live_monitor_active:
            return  # don't override the live counter text
        self.msat_status_var.set(f"MSAT: {name}" + ("  (ready - arm monitor)" if online else ""))
        try:
            self.btn_live.configure(state=("normal" if online else "disabled"))
        except Exception:
            pass

    def toggle_live_monitor(self):
        if self.live_monitor_active:
            self.stop_live_monitor()
        else:
            self.start_live_monitor()

    def start_live_monitor(self):
        if not _HAS_WS:
            messagebox.showerror("Live Monitor",
                                 "Python package 'websocket-client' is required.\n\n"
                                 "Install it then restart:\n    pip install websocket-client")
            return
        self.stop_monitoring()
        self.live_rows = []
        self.manual_excluded = set()
        self.source_type = "OFFLINE"; self.source_path = ""
        self.file_label_var.set(f"LIVE: MSAT {self.msat_ip}")
        self.live_monitor_active = True
        self.live_phase = "armed"   # armed -> capturing -> armed (seamless)
        self._set_ui_locked(True)
        try: self.btn_live.configure(text="■ Stop Live Monitor", bootstyle="danger")
        except Exception: pass
        self.live_thread = threading.Thread(target=self.live_monitor_loop, daemon=True)
        self.live_thread.start()

    def stop_live_monitor(self):
        self.live_monitor_active = False
        if self.live_thread and self.live_thread.is_alive() and \
           threading.current_thread() is not self.live_thread:
            self.live_thread.join(timeout=2.5)
        self._set_ui_locked(False)
        self._live_monitor_finished_ui()

    def _live_monitor_finished_ui(self):
        try: self.btn_live.configure(text="▶ Live Monitor", bootstyle="success")
        except Exception: pass

    def _set_ui_locked(self, locked):
        # While live-monitoring, grey out every interactive control so only
        # the live graph updates. Exceptions stay enabled so the user can
        # exit: Stop button, Local File, drag-drop zone, Check MSAT.
        skip = set()
        for w in (getattr(self, 'btn_live', None), getattr(self, 'btn_local_file', None),
                  getattr(self, 'btn_check', None), getattr(self, '_dz', None)):
            if w is not None:
                skip.add(id(w))
        roots = [getattr(self, 'config_panel', None), getattr(self, 'results_panel', None)]
        interactive = (ttk.Button, ttk.Entry, ttk.Spinbox, ttk.Combobox,
                       ttk.Checkbutton, ttk.Radiobutton)
        def walk(widget):
            for child in widget.winfo_children():
                if id(child) not in skip and isinstance(child, interactive):
                    try:
                        child.state(['disabled'] if locked else ['!disabled'])
                    except Exception:
                        pass
                walk(child)
        for root in roots:
            if root is not None:
                try: walk(root)
                except Exception: pass

    def live_monitor_loop(self):
        # LISTEN to the device WebSocket (/ws) - the same 2 Hz telemetry the
        # web dashboard receives. No HTTP polling, so it adds almost no load.
        # Seamless state machine, never auto-stops:
        #   armed     : wait for a run; keep showing the previous run's graph.
        #   capturing : RUNNING/REC -> accumulate every message (full 2 Hz);
        #               redraw is throttled to ~2s so the PC isn't hammered.
        #               When the run ends, re-arm and keep the finished graph
        #               until the NEXT run starts (then clear -> seamless).
        # Only "Stop Live Monitor" or loading a file leaves this loop.
        ws = None
        last_plot = 0.0
        url = f"ws://{self.msat_ip}/ws"
        while self.live_monitor_active:
            try:
                if ws is None:
                    self.after(0, lambda: self.msat_status_var.set("MSAT: connecting (WebSocket)..."))
                    ws = websocket.create_connection(url, timeout=6)
                    ws.settimeout(6)
                msg = ws.recv()
                if not msg:
                    continue
                d = json.loads(msg)
                state = int(d.get("state", -1))
                running = state in (2, 4)

                if self.live_phase == "armed":
                    if running:
                        self.live_rows = []          # fresh run -> clear old graph
                        self.live_phase = "capturing"
                    else:
                        self.after(0, lambda s=state: self.msat_status_var.set(
                            f"MSAT: {self._state_name(s)}  (armed - waiting for next run)"))

                if self.live_phase == "capturing":
                    if running:
                        self.live_rows.append({
                            "Weightloss": float(d.get("loss", 0)),
                            "pH": float(d.get("ph", 0)),
                            "Volt": float(d.get("volt", 0)),
                            "EC": float(d.get("ec", 0)),
                            "Temp": float(d.get("temp", 0)),
                            "R": int(float(d.get("r", 0))),
                            "G": int(float(d.get("g", 0))),
                            "B": int(float(d.get("b", 0))),
                        })
                        n = len(self.live_rows)
                        uniq = len({round(rw["Weightloss"], 3) for rw in self.live_rows})
                        self.after(0, lambda s=state, n=n: self.msat_status_var.set(
                            f"MSAT: {self._state_name(s)}  (live: {n} pts @2Hz)"))
                        now = time.time()
                        if uniq >= 2 and (now - last_plot) >= 2.0:
                            last_plot = now
                            self.after(0, self._live_update_plot)
                    else:
                        # run finished -> final redraw, keep graph, re-arm
                        self.live_phase = "armed"
                        n = len(self.live_rows)
                        self.after(0, self._live_update_plot)
                        self.after(0, lambda n=n: self.msat_status_var.set(
                            f"MSAT: run finished ({n} pts kept) - waiting for next run"))
            except websocket.WebSocketTimeoutException:
                continue  # no push within timeout - keep listening
            except Exception:
                try:
                    if ws: ws.close()
                except Exception: pass
                ws = None
                self.after(0, lambda: self.msat_status_var.set("MSAT: WebSocket dropped (reconnecting)"))
                time.sleep(2.0)
        try:
            if ws: ws.close()
        except Exception: pass
        self.after(0, self._live_monitor_finished_ui)

    def _live_update_plot(self):
        if not self.live_rows:
            return
        try:
            df = pd.DataFrame(self.live_rows)
            df['sec'] = range(len(df))
            self.df_raw = df
            self.calculate_derived_data(analyze=False)
        except Exception as e:
            print(f"[live] plot update failed: {e}")

    def start_monitor_thread(self):
        self.monitor_thread = threading.Thread(target=self.monitor_loop, daemon=True)
        self.monitor_thread.start()

    def monitor_loop(self):
        # One-shot load: synced data files are complete & static, so there's
        # no need to poll. (The old 15s network/file watch was removed - live
        # data is viewed via the device web dashboard / Live Monitor instead.)
        try:
            with open(self.source_path, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
            self.led_state = "loading"
            self.process_raw_data(lines)
            self.last_line_count = len(lines)
            self.led_state = "ok"
        except Exception as e:
            print(e); self.led_state = "error"
        finally:
            self.is_monitoring = False

    led_state = "off"; led_blink = False
    def update_status_led(self):
        c = "gray"
        if self.led_state == "ok": c = "#00B0FF"
        elif self.led_state == "loading":
            self.led_blink = not self.led_blink
            c = "#00E676" if self.led_blink else "gray"
        elif self.led_state == "idle": c = "#FF1744"
        elif self.led_state == "error": c = "black"
        self.led_canvas.itemconfig(self.led_id, fill=c)
        self.after(500, self.update_status_led)

    def process_raw_data(self, lines):
        try:
            s = io.StringIO("".join(lines))
            df = pd.read_csv(s)
            df.columns = df.columns.str.strip()
            for c in ['Weightloss', 'Weight', 'pH', 'EC', 'Temp', 'R', 'G', 'B', 'Volt']:
                if c in df.columns: df[c] = pd.to_numeric(df[c], errors='coerce')
            if 'Timestamp' in df.columns:
                try:
                    df['ts_obj'] = pd.to_datetime(df['Timestamp'], format='mixed')
                    df['sec'] = (df['ts_obj'] - df['ts_obj'].iloc[0]).dt.total_seconds()
                except: df['sec'] = df.index
            else: df['sec'] = df.index
            self.df_raw = df
            self.after(0, self.update_raw_tree_ui)
            self.after(0, self.calculate_derived_data_ui)
        except Exception as e: print(f"Parse Error: {e}")

    def update_raw_tree_ui(self):
        if self.df_raw is None: return
        self.tree_raw.delete(*self.tree_raw.get_children())
        view_df = self.get_table_view_df(self.df_raw)
        disp_cols = list(view_df.columns)
        if 'ts_obj' in disp_cols: disp_cols.remove('ts_obj')
        self.tree_raw["columns"] = disp_cols
        for c in disp_cols:
            self.tree_raw.heading(c, text=c); self.tree_raw.column(c, width=70)
        for r in view_df.to_numpy():
            safe_r = [f"{x:.2f}" if isinstance(x, float) else str(x) for x in r]
            self.tree_raw.insert("", "end", values=safe_r)
        self.tree_raw.yview_moveto(0.0); self.tree_raw.xview_moveto(0.0)

    def calculate_derived_data_ui(self):
        # If a config sidecar was just loaded, also re-run the EP analysis so
        # trendlines/peaks come back exactly as they were saved.
        analyze = bool(getattr(self, '_auto_analyze_pending', False))
        if analyze:
            self._auto_analyze_pending = False
        self.calculate_derived_data(analyze=analyze)

    # ============================================================
    # DERIVED DATA + ANALYSIS — VERBATIM ALGORITHMS
    # ============================================================
    def _apply_data_cleaning(self, df):
        # Removes whole rows so the result is consistent across plots, the
        # Calculated/Raw tables and every export.
        #  1) Manual deletes: rows the user clicked off on the dashboard.
        #  2) Clean hiccup: single-sample glitches on the X-axis source
        #     (Weightloss) - e.g. a value that spikes up then reverts on the
        #     next sample (idx261: 26.82 -> 31.40 -> 27.04). These corrupt the
        #     titrant axis and make np.gradient explode on EVERY channel at
        #     that point, which is why per-channel y-despiking could not fix
        #     it. Hampel on Weightloss is robust and edge-preserving (the real
        #     monotonic rise is kept; only the lone reverting spike is cut).
        if df is None or len(df) == 0:
            return df
        if self.manual_excluded:
            df = df.loc[~df.index.isin(self.manual_excluded)]
        if self.clean_hiccup_on.get() and 'Weightloss' in df.columns and len(df) >= 5:
            wl = pd.to_numeric(df['Weightloss'], errors='coerce').to_numpy(dtype=float)
            cleaned = Algo.hampel(wl, win=7, n_sig=float(self.hiccup_nsig.get()))
            tol = 1e-6
            keep = ~(np.isfinite(wl) & (np.abs(wl - cleaned) > tol))
            df = df.loc[keep]
        return df

    def calculate_derived_data(self, analyze=False):
        if self.df_raw is None: return
        self.analysis_done = False
        df = self.df_raw.copy()
        df = self._apply_data_cleaning(df)
        if df is None or len(df) == 0: return
        self.df_raw_clean = df.copy()
        d = self.density_var.get()
        if d <= 0: d = 1.0

        drop_rate_mls = None
        try:
            if 'Weightloss' in df.columns and 'sec' in df.columns and len(df) > 1:
                t = pd.to_numeric(df['sec'], errors='coerce')
                w = pd.to_numeric(df['Weightloss'], errors='coerce')
                valid = t.notna() & w.notna()
                if valid.sum() >= 2:
                    t_np = t[valid].to_numpy(dtype=float); w_np = w[valid].to_numpy(dtype=float)
                    order = np.argsort(t_np); t_np = t_np[order]; w_np = w_np[order]
                    dt = np.diff(t_np); keep = np.concatenate(([True], dt > 1e-9))
                    t_np = t_np[keep]; w_np = w_np[keep]
                    if len(t_np) >= 2:
                        slope_gps, _ = np.polyfit(t_np, w_np, 1)
                        if np.isfinite(slope_gps): drop_rate_mls = max(0.0, float(slope_gps) / d)
        except Exception: drop_rate_mls = None
        self.drop_rate_var.set("Drop rate = N/A" if drop_rate_mls is None
                               else f"Drop rate = {drop_rate_mls:.4f} mL/s")

        if 'Weightloss' in df.columns:
            df['Weightloss'] = df['Weightloss'].fillna(0.0)
            df['Vol_mL'] = df['Weightloss'] / d
        elif 'Vol' in df.columns: df['Vol_mL'] = df['Vol']
        else: df['Vol_mL'] = 0.0

        c_m = self.conc_var.get()
        df['Mol'] = (df['Vol_mL'] / 1000.0) * c_m

        mode = self.xaxis_var.get()
        if mode == "mL": df['X_Axis'] = df['Vol_mL']
        elif mode == "mol": df['X_Axis'] = df['Mol']
        elif mode == "g":
            df['X_Axis'] = df['Weightloss'] if 'Weightloss' in df.columns else df['Vol_mL']
        else: df['X_Axis'] = df['sec']

        df = df.sort_values(by='X_Axis')
        dx = df['X_Axis'].diff()
        df = df.loc[dx.isna() | (dx > 1e-9)]

        if self.ph_autoscale_active and 'Volt' in df.columns and len(df) > 5:
            n = 5
            v_start = df['Volt'].iloc[:n].mean()
            v_end = df['Volt'].iloc[-n:].mean()
            p_start = self.phi_var.get(); p_end = self.phf_var.get()
            if abs(v_end - v_start) > 0.001:
                m = (p_end - p_start) / (v_end - v_start)
                c = p_start - m * v_start
                df['pH_Scaled'] = m * df['Volt'] + c
            else: df['pH_Scaled'] = df['pH']
        else:
            df['pH_Scaled'] = df['pH'] if 'pH' in df.columns else 0

        if all(x in df.columns for x in ['R', 'G', 'B']):
            R = df['R'].values; G = df['G'].values; B = df['B'].values
            if self.de_mode.get() == "RGB Eucl.":
                R0, G0, B0 = R[0], G[0], B[0]
                df['dE'] = np.sqrt((R - R0) ** 2 + (G - G0) ** 2 + (B - B0) ** 2)
            else:
                rgb = df[['R', 'G', 'B']].values.astype(float) / 65535.0
                rgb = np.clip(rgb, 0, 1)
                try:
                    lab = color.rgb2lab(rgb.reshape(-1, 1, 3)).reshape(-1, 3)
                    L0, a0, b0 = lab[0]
                    df['dE'] = np.sqrt((lab[:, 0] - L0) ** 2 + (lab[:, 1] - a0) ** 2 + (lab[:, 2] - b0) ** 2)
                except: df['dE'] = 0
        else: df['dE'] = 0

        x = df['X_Axis'].values
        deriv_win = self.algo["sm_deriv"].get()
        def get_d1_d2(y_raw, win):
            y_sm = Algo.smooth_savgol(y_raw, win)
            with np.errstate(divide='ignore', invalid='ignore'):
                d1_raw = np.gradient(y_sm, x)
                d1 = Algo.smooth_savgol(d1_raw, deriv_win)
                d2_raw = np.gradient(d1, x)
                d2 = Algo.smooth_savgol(d2_raw, deriv_win)
            return y_sm, d1, d2

        # The ~32 mL glitch is one bad sample row that shows up on every
        # channel, so Hampel-despike pH/EC/Temp the same way (edge-preserving:
        # the real inflection/step is kept, only single-sample outliers go).
        ph_clean = Algo.hampel(df['pH_Scaled'].values, win=7, n_sig=3.0)
        df['Sm_pH'], df['d1_pH'], df['d2_pH'] = get_d1_d2(ph_clean, self.algo["sm_ph"].get())
        df['Sm_Col'], df['d1_Col'], df['d2_Col'] = get_d1_d2(df['dE'].values, self.algo["sm_col"].get())
        if 'EC' in df.columns:
            df['EC_mS'] = df['EC'] / 1000.0
            # Remove isolated EC drop-out/glitch spikes (e.g. the jump near
            # ~32 mL) before Savitzky-Golay. Hampel is edge-preserving so the
            # real inflection is kept; only single-sample outliers are fixed.
            ec_clean = Algo.hampel(df['EC_mS'].values, win=7, n_sig=3.0)
            df['Sm_EC'], df['d1_EC'], df['d2_EC'] = get_d1_d2(ec_clean, self.algo["sm_ec"].get())
        else:
            df['EC_mS'] = 0; df['Sm_EC'] = 0; df['d1_EC'] = 0; df['d2_EC'] = 0
        self._apply_ec_unit_auto_from_df(df)
        if 'Temp' in df.columns:
            temp_clean = Algo.hampel(df['Temp'].values, win=7, n_sig=3.0)
            df['Sm_Temp'], df['d1_T'], df['d2_T'] = get_d1_d2(temp_clean, self.algo["sm_temp"].get())
        else:
            df['Temp'] = 0; df['Sm_Temp'] = 0; df['d1_T'] = 0; df['d2_T'] = 0

        self.df_cal = df
        self.update_cal_tree()
        self.refresh_plots()
        if analyze: self.perform_ep_analysis()

    def update_cal_tree(self):
        if self.df_cal is None: return
        self.tree_cal.delete(*self.tree_cal.get_children())
        view_df = self.get_table_view_df(self.df_cal)
        cols = ['sec', 'Weightloss', 'X_Axis', 'Vol_mL', 'pH_Scaled', 'dE', 'EC_mS', 'Temp',
                'Sm_pH', 'd1_pH', 'd2_pH', 'd1_Col', 'd2_Col', 'd1_EC', 'd2_EC', 'd1_T', 'd2_T']
        self.tree_cal["columns"] = cols
        for c in cols:
            self.tree_cal.heading(c, text=c)
            self.tree_cal.column(c, width=80 if 'd' not in c else 60)
        for i, r in view_df.iterrows():
            vals = [f"{r.get(c, 0):.3f}" for c in cols]
            self.tree_cal.insert("", "end", values=vals)
        self.tree_cal.yview_moveto(0.0); self.tree_cal.xview_moveto(0.0)

    def update_graph_only(self): self.refresh_plots()

    def _recompute_cleaning(self):
        # Re-run the full derived pipeline so clean-hiccup / sensitivity
        # changes propagate to plots, tables and exports.
        if self.df_raw is None: return
        self.calculate_derived_data(analyze=self.analysis_done)

    def _reset_manual_deletes(self):
        if not self.manual_excluded: return
        self.manual_excluded.clear()
        if self.df_raw is not None:
            self.calculate_derived_data(analyze=self.analysis_done)

    def apply_ph_scale(self):
        self.ph_autoscale_active = True
        self.btn_scale.configure(bootstyle="success", text="✓ Applied")
        self.calculate_derived_data(analyze=False)

    def on_axis_entry_change(self, key):
        try:
            minv = self.yaxis[f"{key}_min"].get().strip()
            maxv = self.yaxis[f"{key}_max"].get().strip()
        except Exception: return
        if minv != "" or maxv != "":
            self.yaxis[f"{key}_auto"].set(False)
            if key == "pH":
                self.ph_fixed_mode.set(False); self.update_ph_fixed_button()
        else:
            self.yaxis[f"{key}_auto"].set(True)
        self.update_axis_auto_button(key); self.update_graph_only()

    def set_axis_auto(self, key, on=True):
        try:
            if key == "pH" and on:
                self.ph_fixed_mode.set(False); self.update_ph_fixed_button()
            self.yaxis[f"{key}_auto"].set(on)
            if on:
                self.yaxis[f"{key}_min"].set(""); self.yaxis[f"{key}_max"].set("")
        except Exception: return
        self.update_axis_auto_button(key); self.update_graph_only()

    def run_analysis(self):
        self.calculate_derived_data(analyze=True)

    # ---- EP analysis (verbatim) ----
    def perform_ep_analysis(self):
        if self.df_cal is None: return
        self.found_eps = {}; self.global_anchors = []; self.trendlines_data = {}
        df = self.df_cal; x = df['X_Axis'].values
        target_peaks = self.peak_sets["num_peak"].get()
        span_pct = self.peak_sets["int_span"].get()
        skip_pct = self.peak_sets["int_skip"].get()
        poly = self.peak_sets["int_poly"].get()
        slope_thresh = self.peak_sets["slope_diff"].get()
        p_prom = self.algo["prom_factor"].get()
        p_ht = self.algo["ht_factor"].get()
        p_dist = self.algo["dist_factor"].get()
        main_s = self.peak_sets["main_sensor"].get()

        if main_s == "Manual":
            try: ep_n_val = float(self.peak_sets["manual_guess"].get())
            except: ep_n_val = None
        else:
            if main_s == "pH": d1_data = df['d1_pH'].values
            elif main_s == "Color": d1_data = df['d1_Col'].values
            elif main_s == "EC": d1_data = df['d2_EC'].values
            else: d1_data = df['d1_T'].values
            peaks_idx = Algo.find_peaks_1st(x, np.abs(d1_data), p_prom, p_ht, p_dist, target_peaks)
            ep_n_val = None
            if len(peaks_idx) > 0:
                valid_pks = [x[i] for i in peaks_idx if x[i] > (x.max() * 0.1)]
                if valid_pks: ep_n_val = max(valid_pks)

        if ep_n_val is None:
            self.found_eps = {}; self.update_summary_tree(); self.refresh_plots(); return

        expected_anchors = [ep_n_val * (i / target_peaks) for i in range(1, target_peaks + 1)]
        self.global_anchors = expected_anchors
        user_win_pct = self.peak_sets["search_win"].get() / 100.0

        for name, col_y, key_meth, d1_col, d2_col in [
            ("pH", "Sm_pH", "method_ph", "d1_pH", "d2_pH"),
            ("Color", "Sm_Col", "method_col", "d1_Col", "d2_Col"),
            ("EC", "Sm_EC", "method_ec", "d1_EC", "d2_EC"),
            ("Temp", "Sm_Temp", "method_temp", "d1_T", "d2_T"),
        ]:
            method = self.peak_sets[key_meth].get()
            y_sm = df[col_y].values
            final_eps = [None] * target_peaks
            d_check = df[d2_col].values if method == 2 else df[d1_col].values
            global_max_d = np.max(np.abs(d_check)) if len(d_check) > 0 else 0

            for i, target_val in enumerate(expected_anchors):
                if name == "Temp" and self.temp_last_only.get() and i != (target_peaks - 1):
                    final_eps[i] = None; continue
                win_min = target_val * (1.0 - user_win_pct)
                win_max = target_val * (1.0 + user_win_pct)
                mask_idx = np.where((x >= win_min) & (x <= win_max))[0]
                if len(mask_idx) < 5: continue
                x_win = x[mask_idx]
                res = None
                if method == 3:
                    algo_ret = Algo.find_intersection(x, y_sm, target_val, span_pct, skip_pct, poly, slope_thresh)
                    if algo_ret:
                        res, (pL, x_L), (pR, x_R) = algo_ret
                        if name not in self.trendlines_data: self.trendlines_data[name] = []
                        self.trendlines_data[name].append({'pL': pL, 'xL': x_L, 'pR': pR, 'xR': x_R, 'root': res})
                else:
                    cands = []
                    if method == 2:
                        d2 = df[d2_col].values[mask_idx]
                        pks = Algo.find_peaks_1st(x_win, np.abs(d2), p_prom, p_ht, p_dist, 1)
                        cands = [x_win[k] for k in pks]
                    else:
                        d1 = df[d1_col].values[mask_idx]
                        pks = Algo.find_peaks_1st(x_win, np.abs(d1), p_prom, p_ht, p_dist, 1)
                        cands = [x_win[k] for k in pks]
                    if cands:
                        closest_cand = min(cands, key=lambda c: abs(c - target_val))
                        idx_in_full = (np.abs(x - closest_cand)).argmin()
                        cand_h = np.abs(d_check[idx_in_full])
                        res = closest_cand if cand_h >= (global_max_d * p_ht) else None
                if res and (res < win_min or res > win_max): res = None
                final_eps[i] = res
            self.found_eps[name] = final_eps

        self.analysis_done = True
        self.update_summary_tree(); self.refresh_plots()

    def update_summary_tree(self):
        self.tree.delete(*self.tree.get_children())
        num = self.peak_sets["num_peak"].get()
        x = self.df_cal['X_Axis'].values
        y_ph = self.df_cal['Sm_pH'].values
        sensors = ["pH", "Color", "EC", "Temp"]
        for i in range(num):
            for s in sensors:
                if (i, s) not in self.ep_visibility: self.ep_visibility[(i, s)] = True
                if (i, s) not in self.ep_visibility_applied: self.ep_visibility_applied[(i, s)] = True

        if 'Sm_Temp' in self.df_cal.columns:
            sm_t = self.df_cal['Sm_Temp'].values
            t_min = np.nanmin(sm_t)
            if self.dt_mode.get() == "ep_min":
                temp_eps = self.found_eps.get("Temp", [])
                visible_eps = [ep for i, ep in enumerate(temp_eps)
                               if ep is not None and self.ep_visibility_applied.get((i, "Temp"), True)]
                if visible_eps:
                    dt = float(np.interp(visible_eps[-1], x, sm_t)) - t_min
                else:
                    dt = np.nanmax(sm_t) - t_min
            else:
                dt = np.nanmax(sm_t) - t_min
            self.tree.insert("", "end", values=('', "Delta T", f"{dt:.2f}"))

        for i in range(num):
            prefix = f"{i+1}-"
            for s in sensors:
                val = "-"; show_mark = ''
                if s in self.found_eps and i < len(self.found_eps[s]):
                    v = self.found_eps[s][i]
                    if v is not None:
                        if self.ep_visibility_applied.get((i, s), True):
                            show_mark = '✓'; val = f"{v:.2f}"
                        else:
                            show_mark = '✗'; val = "-"
                self.tree.insert("", "end", values=(show_mark, f"{prefix}EPx-{s}", val))

            for s, col in zip(sensors, ['pH_Scaled', 'dE', 'EC_mS', 'Temp']):
                val = "-"
                if s in self.found_eps and i < len(self.found_eps[s]):
                    ep = self.found_eps[s][i]
                    if ep is not None and self.ep_visibility_applied.get((i, s), True):
                        val = f"{np.interp(ep, x, self.df_cal[col]):.2f}"
                self.tree.insert("", "end", values=('', f"{prefix}EPy-{s}", val))

            for s in sensors:
                val = "-"
                if s in self.found_eps and i < len(self.found_eps[s]):
                    ep = self.found_eps[s][i]
                    if ep is not None and self.ep_visibility_applied.get((i, s), True):
                        if i == 0: target = ep / 2.0
                        else:
                            prev_ep = self.found_eps[s][i - 1] if (i - 1 < len(self.found_eps[s]) and self.found_eps[s][i - 1]) else None
                            if prev_ep: target = (prev_ep + ep) / 2.0
                            elif i < len(self.global_anchors) and i > 0: target = (self.global_anchors[i - 1] + ep) / 2.0
                            else: target = ep * 0.75
                        if target: val = f"{np.interp(target, x, y_ph):.2f}"
                self.tree.insert("", "end", values=('', f"{prefix}pKa-{s}", val))

        self.tree.insert("", "end", values=('', "---", "---"))

    # ============================================================
    # PLOTTING — verbatim from original
    # ============================================================
    def refresh_plots(self):
        self.fig.clear()
        self.draw_all_graphs(self.fig, is_export=False)
        self.canvas.draw()

    def draw_all_graphs(self, fig, is_export):
        # Tighter horizontal spacing on exported figures (the right column
        # shifts slightly toward the left column, closing the visible gap).
        # On-screen layout is unchanged.
        _wspace = 0.32 if is_export else 0.40
        fig.subplots_adjust(left=0.20, right=0.95, top=0.93, bottom=0.15, wspace=_wspace, hspace=0.25)
        gs = fig.add_gridspec(2, 2)
        mode = self.xaxis_var.get()
        if mode == "mL": xlab = "Titrant (mL)"
        elif mode == "mol": xlab = "Titrant (mol)"
        elif mode == "g": xlab = "Titrant (g)"
        else: xlab = "Time (s)"
        if is_export:
            fs = self.vis["font"].get() * 1.4; lw = 6.0; dlw = 1.5
            pt = self.vis["pt_size"].get() * 6.0; sw = 1.2; main_line_alpha = 0.45
        else:
            fs = self.vis["font"].get(); lw = 2.0; dlw = 1.0
            pt = self.vis["pt_size"].get(); sw = 1.0; main_line_alpha = 0.6
        if self.df_cal is None: return
        left_axes, right_axes = [], []
        left_axes.extend(self.plot_sensor(fig, gs[0, 0], "pH", self.df_cal['pH_Scaled'].values,
                                          self.df_cal['d1_pH'].values, self.df_cal['d2_pH'].values,
                                          "pH", "#1976D2", "#BBDEFB", is_export, fs, pt, lw, dlw, sw, xlab, main_line_alpha))
        right_axes.extend(self.plot_sensor(fig, gs[0, 1], "Color", self.df_cal['dE'].values,
                                            self.df_cal['d1_Col'].values, self.df_cal['d2_Col'].values,
                                            r"Color Change ($\Delta E$)", "#388E3C", "#C8E6C9",
                                            is_export, fs, pt, lw, dlw, sw, xlab, main_line_alpha))
        left_axes.extend(self.plot_sensor(fig, gs[1, 0], "EC", self.df_cal['EC_mS'].values,
                                           self.df_cal['d1_EC'].values, self.df_cal['d2_EC'].values,
                                           "EC (mS/cm)", "#F57F17", "#FFF9C4", is_export, fs, pt, lw, dlw, sw, xlab, main_line_alpha))
        right_axes.extend(self.plot_sensor(fig, gs[1, 1], "Temp", self.df_cal['Temp'].values,
                                            self.df_cal['d1_T'].values, self.df_cal['d2_T'].values,
                                            "Temperature (°C)", "#E91E63", "#F8BBD0",
                                            is_export, fs, pt, lw, dlw, sw, xlab, main_line_alpha))
        if left_axes: fig.align_ylabels(left_axes)
        if right_axes: fig.align_ylabels(right_axes)

    def plot_sensor(self, fig, gs_pos, name, y_raw, d1, d2, ylab_top, col, fill_col,
                    is_export, fs, pt, lw, dlw, sw, xlab, main_alpha):
        gs_inner = gs_pos.subgridspec(2, 1, height_ratios=[4, 1], hspace=0.15)
        ax1 = fig.add_subplot(gs_inner[0])
        ax2 = fig.add_subplot(gs_inner[1], sharex=ax1)
        x = self.df_cal['X_Axis'].values
        win = 15
        if name == "pH": win = self.algo["sm_ph"].get()
        elif name == "Color": win = self.algo["sm_col"].get()
        elif name == "EC": win = self.algo["sm_ec"].get()
        else: win = self.algo["sm_temp"].get()
        y_sm = Algo.smooth_savgol(y_raw, win)
        method = 1
        if name == "pH": method = self.peak_sets["method_ph"].get()
        elif name == "Color": method = self.peak_sets["method_col"].get()
        elif name == "EC": method = self.peak_sets["method_ec"].get()
        else: method = self.peak_sets["method_temp"].get()

        y_disp = y_sm.copy(); y_raw_disp = y_raw.copy()
        d1_disp = d1.copy(); d2_disp = d2.copy()
        ylab = ylab_top
        y_factor = 1.0  # used to scale trendlines onto the displayed EC unit
        if name == "EC":
            unit = self.ec_unit_var.get()
            if unit == "uS/cm": factor = 1000.0; ylab = "EC (uS/cm)"
            else: factor = 1.0; ylab = "EC (mS/cm)"
            y_factor = factor
            y_disp *= factor; y_raw_disp *= factor; d1_disp *= factor; d2_disp *= factor

        ax1.plot(x, y_disp, '-', c=col, lw=lw, alpha=main_alpha, zorder=1)
        sc = ax1.scatter(x, y_raw_disp, c='black', s=pt, alpha=1.0, zorder=2,
                         picker=(not is_export), pickradius=5)
        # Map each plotted point back to its original df_raw row so a click
        # can delete exactly that sample (df_cal keeps the original index
        # through copy/sort/dedup).
        try:
            sc._orig_idx = self.df_cal.index.to_numpy()
        except Exception:
            sc._orig_idx = None

        if self.show_trendlines.get() and name in self.trendlines_data:
            # Thicker trendlines; scale by y_factor so EC trendlines (fit in
            # mS) land on the displayed unit (they were drawn ~0 on a uS axis).
            tl_lw = 4.0 if is_export else 2.6
            tl_dw = 2.0 if is_export else 1.3
            for item in self.trendlines_data[name]:
                y_l_fit = np.polyval(item['pL'], item['xL']) * y_factor
                ax1.plot(item['xL'], y_l_fit, color='red', linewidth=tl_lw, zorder=5)
                y_r_fit = np.polyval(item['pR'], item['xR']) * y_factor
                ax1.plot(item['xR'], y_r_fit, color='red', linewidth=tl_lw, zorder=5)
                y_root_l = np.polyval(item['pL'], item['root']) * y_factor
                y_root_r = np.polyval(item['pR'], item['root']) * y_factor
                ax1.plot([item['xL'][-1], item['root']], [y_l_fit[-1], y_root_l], color='red', linewidth=tl_dw, linestyle=':')
                ax1.plot([item['xR'][0], item['root']], [y_r_fit[0], y_root_r], color='red', linewidth=tl_dw, linestyle=':')

        with np.errstate(divide='ignore', invalid='ignore'):
            if method == 2:
                ax2.plot(x, d2_disp, c=col, lw=dlw)
                ax2.axhline(0, color='black', lw=sw, linestyle=':')
                ax2.fill_between(x, 0, d2_disp, color=fill_col, alpha=0.5)
                bot_lab = {"pH": r"$d^2\text{pH}/dV^2$", "Color": r"$d^2(\Delta E)/dV^2$",
                           "EC": r"$d^2\text{EC}/dV^2$"}.get(name, r"$d^2\text{T}/dV^2$")
            else:
                ax2.plot(x, d1_disp, c=col, lw=dlw)
                ax2.fill_between(x, 0, d1_disp, color=fill_col, alpha=0.5)
                bot_lab = {"pH": "dpH/dV", "Color": "d(\u0394E)/dV", "EC": "dEC/dV"}.get(name, "dT/dV")

        if name in self.found_eps:
            for idx, ep in enumerate(self.found_eps[name]):
                if ep is not None and self.ep_visibility_applied.get((idx, name), True):
                    ax1.axvline(ep, color='red', ls='--', lw=1.5)
                    ax1.text(ep, 0.90, f"{ep:.2f}", color='red', rotation=90,
                             ha='right', va='center', fontsize=fs * 0.8,
                             transform=ax1.get_xaxis_transform())
        if name == "Temp" and self.show_dt_in_graph.get() and self.analysis_done:
            if self.dt_mode.get() == "ep_min":
                temp_eps = self.found_eps.get("Temp", [])
                visible_eps = [ep for i, ep in enumerate(temp_eps)
                               if ep is not None and self.ep_visibility_applied.get((i, "Temp"), True)]
                if visible_eps:
                    last_ep = visible_eps[-1]
                    t_at_ep = float(np.interp(last_ep, x, y_disp))
                    dt_val = t_at_ep - np.nanmin(y_disp)
                else:
                    dt_val = np.nanmax(y_disp) - np.nanmin(y_disp)
            else:
                dt_val = np.nanmax(y_disp) - np.nanmin(y_disp)
            ax1.text(0.03, 0.97, f"$\\Delta T = {dt_val:.2f}\\degree$C",
                     color='red', fontsize=fs,
                     transform=ax1.transAxes, va='top')

        key_map = {"pH": "pH", "Color": "dE", "EC": "EC", "Temp": "T"}
        k = key_map.get(name, None)
        if k:
            if name == "pH" and self.ph_fixed_mode.get():
                ax1.set_ylim(-0.5, 14.5); ax1.set_yticks([0, 7, 14])
            else:
                auto = self.yaxis.get(f"{k}_auto").get()
                min_s = self.yaxis.get(f"{k}_min").get().strip()
                max_s = self.yaxis.get(f"{k}_max").get().strip()
                min_v = None; max_v = None
                try:
                    if not auto:
                        if min_s != "": min_v = float(min_s)
                        if max_s != "": max_v = float(max_s)
                except: min_v = None; max_v = None
                try:
                    if not auto:
                        if min_v is not None and max_v is not None and min_v < max_v: ax1.set_ylim(min_v, max_v)
                        elif min_v is not None: ax1.set_ylim(bottom=min_v)
                        elif max_v is not None: ax1.set_ylim(top=max_v)
                    else:
                        if len(y_disp) > 0:
                            ymn = np.nanmin(y_disp); ymx = np.nanmax(y_disp)
                            if np.isfinite(ymn) and np.isfinite(ymx):
                                if name == "Color":
                                    # dE: small negative floor; round max UP to a
                                    # whole number then +1 headroom so the curve
                                    # sits lower and EP labels read clearly.
                                    bottom_v = -0.1
                                    top_v = float(np.ceil(ymx) + 1)
                                elif name == "Temp":
                                    # Integer bounds: floor min, ceil max + 2.
                                    bottom_v = float(np.floor(ymn))
                                    top_v = float(np.ceil(ymx) + 2)
                                else:
                                    pad = (ymx - ymn) * 0.06 if ymx > ymn else abs(ymx) * 0.1 + 1.0
                                    bottom_v = ymn - pad; top_v = ymx + pad
                                ax1.set_ylim(bottom_v, top_v)
                                try:
                                    digits_now = max(0, min(6, int(self.yaxis_digits.get(k).get())))
                                    self.yaxis[f"{k}_min"].set(f"{bottom_v:.{digits_now}f}")
                                    self.yaxis[f"{k}_max"].set(f"{top_v:.{digits_now}f}")
                                except Exception: pass
                                # Auto-fill a clean tick interval for dE/EC/Temp
                                # (pH keeps its user 7). Shown in the Interval box.
                                if name in ("Color", "EC", "Temp"):
                                    try:
                                        nv = self._nice_interval(top_v - bottom_v)
                                        if nv is not None and nv > 0:
                                            if float(nv).is_integer():
                                                self.yaxis_interval[k].set(str(int(nv)))
                                            else:
                                                self.yaxis_interval[k].set(f"{nv:g}")
                                    except Exception: pass
                except Exception: pass

        if name == "pH" and self.ph_fixed_mode.get():
            try: ax1.set_yticks([0, 7, 14])
            except Exception: pass

        try:
            digit_key = key_map.get(name, None)
            base_digits = int(self.yaxis_digits[digit_key].get()) if (digit_key and digit_key in self.yaxis_digits) else 0
            # If the y tick interval is a whole number, drop decimals on the
            # tick labels (they would always be ".0"); otherwise honour Digits.
            eff_iv = None
            if k:
                _ivs = self.yaxis_interval.get(k).get().strip()
                if _ivs != "":
                    try: eff_iv = float(_ivs)
                    except Exception: eff_iv = None
            if eff_iv is not None and eff_iv > 0 and float(eff_iv).is_integer():
                base_digits = 0
            self._apply_unique_y_formatter(ax1, base_digits)
        except Exception: pass

        # Y-axis tick interval (blank = auto). Applied last so it overrides
        # the default/auto tick locations (e.g. pH interval 7 -> 0, 7, 14).
        try:
            if k:
                iv_s = self.yaxis_interval.get(k).get().strip()
                if iv_s != "":
                    iv = float(iv_s)
                    if np.isfinite(iv) and iv > 0:
                        ax1.yaxis.set_major_locator(MultipleLocator(iv))
        except Exception: pass

        ax1.set_ylabel(ylab, fontsize=fs)
        ax2.set_ylabel(bot_lab, fontsize=fs * 0.8, labelpad=-15)
        ax2.set_xlabel(xlab, fontsize=fs)
        ax2.yaxis.set_ticklabels([])
        for ax in [ax1, ax2]:
            ax.grid(True, ls=':', alpha=0.6)
            ax.set_xlim(left=0)
            try:
                xm = float(self.xmax_var.get())
                if xm > 0: ax.set_xlim(right=xm)
            except: pass
            for spine in ax.spines.values(): spine.set_linewidth(sw)
            ax.tick_params(axis='both', which='major', labelsize=fs, width=sw, length=6)
        ax1.tick_params(axis='x', labelbottom=False)
        return [ax1, ax2]

    # ---- Summary clipboard / EP selection ----
    def copy_summary_text(self):
        param_col = ["Filename"]
        value_col = [os.path.basename(self.source_path) if getattr(self, 'source_path', None) else ""]
        for item in self.tree.get_children():
            vals = self.tree.item(item)['values']
            if len(vals) >= 3:
                param_col.append(str(vals[1]))
                value_col.append(str(vals[2]))
            elif len(vals) == 2:
                param_col.append(str(vals[0]))
                value_col.append(str(vals[1]))
            else:
                param_col.append(""); value_col.append("")
        lines = [
            "\t".join(param_col),
            "\t".join(value_col),
        ]
        self.clipboard_clear(); self.clipboard_append("\n".join(lines))
        messagebox.showinfo("Copied", "Summary Copied (Transposed)!")

    def on_summary_click(self, event):
        region = self.tree.identify("region", event.x, event.y)
        if region != "cell": return
        col = self.tree.identify_column(event.x)
        if col != "#1": return
        item = self.tree.identify_row(event.y)
        if not item: return
        vals = self.tree.item(item).get('values', [])
        if not vals or len(vals) < 2: return
        param = vals[1]
        if "EPx-" not in param: return
        try:
            parts = param.split('-')
            idx = int(parts[0]) - 1; sensor = parts[2]
        except Exception: return
        key = (idx, sensor)
        found = False
        try:
            found = (sensor in self.found_eps and idx < len(self.found_eps[sensor])
                     and self.found_eps[sensor][idx] is not None)
        except Exception: found = False
        if not found: return
        cur = self.ep_visibility.get(key, True)
        self.ep_visibility[key] = not cur
        new_mark = '✓' if self.ep_visibility[key] else '✗'
        val_cell = vals[2] if len(vals) >= 3 else ''
        self.tree.item(item, values=(new_mark, param, val_cell))

    def apply_ep_selection(self):
        try:
            self.ep_visibility_applied = dict(self.ep_visibility)
            self.update_summary_tree(); self.refresh_plots()
        except Exception: pass

    def copy_data(self, tag):
        if tag == "cal":
            df = self.df_cal
        else:
            # Raw export honours clean-hiccup + manual deletes too.
            df = self.df_raw_clean if self.df_raw_clean is not None else self.df_raw
        if df is None: return
        df.to_clipboard(sep='\t', index=False)
        messagebox.showinfo("Copied", f"{tag.upper()} Data Copied!")

    def copy_img_clipboard(self):
        if self.df_cal is None: return
        try:
            buf = io.BytesIO()
            self.create_export_fig(buf, to_buffer=True)
            buf.seek(0)
            img = Image.open(buf)
            Clipboard.to_clipboard(img)
            messagebox.showinfo("Success", "Copied!")
        except Exception as e: messagebox.showerror("Error", str(e))

    def save_image(self):
        if self.df_cal is None:
            return

        # Determine base path from loaded txt input file
        if getattr(self, 'source_type', None) == "OFFLINE" and self.source_path:
            base_dir  = os.path.dirname(os.path.abspath(self.source_path))
            base_name = os.path.splitext(os.path.basename(self.source_path))[0]
        else:
            messagebox.showwarning("Save Image", "No local input file loaded.\nPlease load a data file first.")
            return

        # Prefix saved files with "-" so they sort separately from the raw
        # data .txt files when the folder is listed alphabetically.
        save_name = "-" + base_name
        # 4 output paths
        out_files = [
            ("PNG  (600 DPI)", save_name + ".png"),
            ("TIFF (600 DPI)", save_name + ".tif"),
            ("SVG  (vector)",  save_name + ".svg"),
            ("PDF  (vector)",  save_name + ".pdf"),
        ]
        full_paths = [os.path.join(base_dir, fn) for _, fn in out_files]
        any_exists = any(os.path.exists(p) for p in full_paths)

        # ── Confirmation dialog ──────────────────────────────────────────
        dlg = tk.Toplevel(self)
        dlg.title("Save Image")
        dlg.resizable(False, False)
        dlg.grab_set()
        dlg.lift()

        ttk.Label(dlg, text="The following files will be saved:",
                  font=("Arial", 10, "bold")).pack(anchor="w", padx=16, pady=(14, 4))

        for (label, fn), fp in zip(out_files, full_paths):
            mark = "  ⚠ already exists" if os.path.exists(fp) else ""
            ttk.Label(dlg, text=f"  •  {label}:  {fn}{mark}",
                      font=("Consolas", 9)).pack(anchor="w", padx=24, pady=1)

        ttk.Label(dlg, text=f"Folder:  {base_dir}",
                  font=("Arial", 8), foreground="gray").pack(anchor="w", padx=20, pady=(6, 2))

        if any_exists:
            ttk.Label(dlg, text="⚠  One or more files already exist and will be overwritten.",
                      font=("Arial", 9), foreground="#e67e00").pack(anchor="w", padx=16, pady=(4, 2))

        btn_frame = ttk.Frame(dlg)
        btn_frame.pack(fill=X, padx=16, pady=(10, 14))

        result = [False]

        def do_save():
            result[0] = True
            dlg.destroy()

        def do_cancel():
            dlg.destroy()

        save_label = "💾 Overwrite" if any_exists else "💾 Save"
        ttk.Button(btn_frame, text=save_label, command=do_save,
                   bootstyle="primary").pack(side=LEFT, padx=(0, 8))
        ttk.Button(btn_frame, text="Cancel", command=do_cancel,
                   bootstyle="secondary-outline").pack(side=LEFT)

        dlg.wait_window()

        if result[0]:
            self._save_all_formats(base_dir, save_name)
            # Also drop the analysis config sidecar so this exact view can
            # be reproduced by loading the same data file again.
            try:
                self._save_config_sidecar(base_dir, base_name)
            except Exception as exc:
                print(f"[config] save failed: {exc}")

    def _save_all_formats(self, base_dir, base_name):
        """Render the figure once and export PNG 600 dpi, TIFF 600 dpi, SVG, PDF."""
        win_w = self.tab_dash.winfo_width()
        win_h = self.tab_dash.winfo_height()
        if win_w < 100:
            win_w, win_h = 1600, 900
        aspect = win_w / win_h
        base_h = 10.0
        fig = Figure(figsize=(base_h * aspect, base_h), dpi=150)
        self.draw_all_graphs(fig, is_export=True)

        errors = []
        saves = [
            (base_name + ".png", dict(dpi=600, bbox_inches="tight")),
            (base_name + ".tif", dict(dpi=600, bbox_inches="tight", format="tiff")),
            (base_name + ".svg", dict(format="svg",  bbox_inches="tight")),
            (base_name + ".pdf", dict(format="pdf",  bbox_inches="tight")),
        ]
        for fn, kwargs in saves:
            fp = os.path.join(base_dir, fn)
            try:
                fig.savefig(fp, **kwargs)
            except Exception as exc:
                errors.append(f"{fn}: {exc}")

        plt.close(fig)

        if errors:
            messagebox.showerror("Save Image",
                                 "Some files could not be saved:\n" + "\n".join(errors))
        else:
            messagebox.showinfo("Save Image",
                                f"4 files saved successfully!\n\nFolder:\n{base_dir}")

    def create_export_fig(self, output, to_buffer=False):
        win_w = self.tab_dash.winfo_width(); win_h = self.tab_dash.winfo_height()
        if win_w < 100: win_w = 1600; win_h = 900
        aspect = win_w / win_h
        base_h = 10; base_w = base_h * aspect
        fig = Figure(figsize=(base_w, base_h), dpi=150)
        self.draw_all_graphs(fig, is_export=True)
        if to_buffer:
            fig.savefig(output, format='png', dpi=300, bbox_inches='tight')
        else:
            ext = os.path.splitext(str(output))[1].lower()
            if ext == '.svg':
                fig.savefig(output, format='svg', bbox_inches='tight')
            elif ext == '.pdf':
                fig.savefig(output, format='pdf', bbox_inches='tight')
            else:
                fig.savefig(output, dpi=300, bbox_inches='tight')

    # ============================================================
    # CONFIG SIDECAR  -<basename>-config.txt
    # ------------------------------------------------------------
    # On Save Image we also drop a JSON sidecar with every relevant UI
    # setting + manual delete list. When the same data file is loaded later,
    # the sidecar is auto-detected and applied so the previous analysis view
    # (smoothing, peak settings, axis ranges/intervals, manual deletes, ...)
    # is restored exactly. The "-" prefix keeps it sorted next to the
    # exported images, away from raw data .txt files.
    # ============================================================
    def _config_sidecar_path(self, data_path):
        base_dir = os.path.dirname(os.path.abspath(data_path))
        base_name = os.path.splitext(os.path.basename(data_path))[0]
        return os.path.join(base_dir, "-" + base_name + "-config.txt")

    def _collect_config(self):
        cfg = {}
        def g(name, var):
            try: cfg[name] = var.get()
            except Exception: pass
        # Axis & scaling, ΔE mode, EC unit
        g("xaxis", self.xaxis_var); g("density", self.density_var); g("conc", self.conc_var)
        g("xmax", self.xmax_var); g("ec_unit", self.ec_unit_var); g("ec_unit_auto", self.ec_unit_auto)
        g("de_mode", self.de_mode); g("ph_fixed_mode", self.ph_fixed_mode)
        # pH calibration
        g("phi", self.phi_var); g("phf", self.phf_var)
        cfg["ph_autoscale_active"] = bool(self.ph_autoscale_active)
        # Smoothing / peak detection / sensor methods / inflection
        cfg["algo"] = {k: self.algo[k].get() for k in self.algo}
        cfg["peak_sets"] = {k: self.peak_sets[k].get() for k in self.peak_sets}
        # Toggles
        g("show_trendlines", self.show_trendlines)
        g("temp_last_only", self.temp_last_only)
        g("show_dt_in_graph", self.show_dt_in_graph)
        g("dt_mode", self.dt_mode)
        # Display tab
        cfg["vis"] = {k: self.vis[k].get() for k in self.vis}
        cfg["yaxis"] = {k: self.yaxis[k].get() for k in self.yaxis}
        cfg["yaxis_digits"] = {k: self.yaxis_digits[k].get() for k in self.yaxis_digits}
        cfg["yaxis_interval"] = {k: self.yaxis_interval[k].get() for k in self.yaxis_interval}
        # Data Processing
        g("clean_hiccup_on", self.clean_hiccup_on)
        g("hiccup_nsig", self.hiccup_nsig)
        cfg["manual_excluded"] = sorted(int(i) for i in self.manual_excluded)
        cfg["_meta"] = {
            "version": "V.Y2026.88.209",
            "saved_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        }
        return cfg

    def _apply_config(self, cfg):
        if not isinstance(cfg, dict): return
        def s(name, var, conv=None):
            if name in cfg:
                try:
                    v = cfg[name]
                    if conv is not None: v = conv(v)
                    var.set(v)
                except Exception: pass
        s("xaxis", self.xaxis_var)
        s("density", self.density_var, float)
        s("conc", self.conc_var, float)
        s("xmax", self.xmax_var, str)
        s("ec_unit", self.ec_unit_var)
        s("ec_unit_auto", self.ec_unit_auto, bool)
        s("de_mode", self.de_mode)
        s("ph_fixed_mode", self.ph_fixed_mode, bool)
        s("phi", self.phi_var, float)
        s("phf", self.phf_var, float)
        if "ph_autoscale_active" in cfg:
            try: self.ph_autoscale_active = bool(cfg["ph_autoscale_active"])
            except Exception: pass
        for d_name in ("algo", "peak_sets", "vis", "yaxis", "yaxis_digits", "yaxis_interval"):
            d = cfg.get(d_name)
            target = getattr(self, d_name, None)
            if isinstance(d, dict) and isinstance(target, dict):
                for k, v in d.items():
                    if k in target:
                        try: target[k].set(v)
                        except Exception: pass
        s("show_trendlines", self.show_trendlines, bool)
        s("temp_last_only", self.temp_last_only, bool)
        s("show_dt_in_graph", self.show_dt_in_graph, bool)
        s("dt_mode", self.dt_mode)
        s("clean_hiccup_on", self.clean_hiccup_on, bool)
        s("hiccup_nsig", self.hiccup_nsig, float)
        if "manual_excluded" in cfg:
            try:
                self.manual_excluded = set(int(i) for i in cfg["manual_excluded"])
            except Exception:
                self.manual_excluded = set()

    def _save_config_sidecar(self, base_dir, base_name):
        p = os.path.join(base_dir, "-" + base_name + "-config.txt")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(self._collect_config(), f, indent=2, ensure_ascii=False)

    def _try_load_config_sidecar(self, data_path):
        try:
            p = self._config_sidecar_path(data_path)
            if not os.path.isfile(p):
                return False
            with open(p, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            self._apply_config(cfg)
            # Tell calculate_derived_data_ui to also run the EP analysis
            # so trendlines/peaks appear exactly as they were when saved.
            self._auto_analyze_pending = True
            print(f"[config] loaded sidecar: {os.path.basename(p)}")
            return True
        except Exception as exc:
            print(f"[config] load failed: {exc}")
            return False

    def _toggle_maximize(self):
        if getattr(self, '_toggle_busy', False):
            return
        self._toggle_busy = True
        self._pre_max_geometry = self.geometry()
        self.state('zoomed')
        self.after(200, self._restore_from_maximize)

    def _restore_from_maximize(self):
        self.state('normal')
        if self._pre_max_geometry:
            self.geometry(self._pre_max_geometry)
        self._toggle_busy = False

    def _startup_layout_fix(self):
        # Do the zoom/restore dance silently: hide the window first so the
        # user sees the app appear once, already at the correct layout, with
        # no full-screen flash. Faster than the user-facing toggle (no 200ms
        # animation wait) since nothing is visible.
        try:
            self.withdraw()
            self._pre_max_geometry = self.geometry()
            self.state('zoomed')
            self.update_idletasks()
            self.state('normal')
            if self._pre_max_geometry:
                self.geometry(self._pre_max_geometry)
            self.update_idletasks()
        finally:
            self.deiconify()
            self.lift()

    def on_close(self):
        self.live_monitor_active = False
        self.stop_monitoring()
        self.destroy()


if __name__ == "__main__":
    # Optional CLI:
    #   path/to/data.txt        auto-load this file after launch
    #   --smoke-exit N          close window after N seconds (non-zero exit if it crashed first)
    #   --smoke-verify          after load+analysis, print status of df_raw/df_cal/found_eps to stdout
    _auto_path = None
    _smoke_exit_s = None
    _smoke_verify = False
    _args = sys.argv[1:]
    i = 0
    while i < len(_args):
        a = _args[i]
        if a == "--smoke-exit" and i + 1 < len(_args):
            try: _smoke_exit_s = float(_args[i + 1])
            except Exception: _smoke_exit_s = None
            i += 2; continue
        if a == "--smoke-verify":
            _smoke_verify = True; i += 1; continue
        if _auto_path is None and os.path.isfile(a):
            _auto_path = a
        i += 1

    app = MSAT_Redesign()
    app.after(50, app._startup_layout_fix)

    if _auto_path:
        app.after(800, lambda: app._load_file_path(_auto_path))
        # Force analysis after the monitor thread + derived-data pipeline has had time to run.
        app.after(5000, lambda: app.run_analysis())

    if _smoke_verify:
        def _verify():
            n_raw = 0 if app.df_raw is None else len(app.df_raw)
            n_cal = 0 if app.df_cal is None else len(app.df_cal)
            n_eps = sum(1 for sens, lst in app.found_eps.items() for v in (lst or []) if v is not None)
            print(f"SMOKE: df_raw={n_raw} rows, df_cal={n_cal} rows, found_eps total={n_eps}")
            print(f"SMOKE: drop_rate='{app.drop_rate_var.get()}'")
            print(f"SMOKE: file_label='{app.file_label_var.get()}'")
            for sens, lst in app.found_eps.items():
                vals = [f"{v:.3f}" if v is not None else "None" for v in (lst or [])]
                print(f"SMOKE: {sens} EPs = [{', '.join(vals)}]")
        # Run verify just before exit so analysis has time to finish.
        app.after(int((_smoke_exit_s or 30) * 1000) - 1500, _verify)

    if _smoke_exit_s and _smoke_exit_s > 0:
        app.after(int(_smoke_exit_s * 1000), app.on_close)
    app.mainloop()
