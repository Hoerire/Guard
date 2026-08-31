package com.example.guard;

import android.app.Activity;
import android.app.Dialog;
import android.os.Build;
import android.os.Bundle;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.animation.OvershootInterpolator;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.*;

/**
 * Guard - 自由启停
 * 前台应用感知守护进程，通过系统事件日志监听前台应用变化，
 * 命中目标即批量禁用指定应用，离开即恢复。
 */
public class MainActivity extends Activity {

    // ==================== 常量 ====================

    static final String ASSET = "guard_arm64-v8a";
    static final String EXE_NAME = "guard";

    // 配色
    static final int PRIMARY       = 0xFF5B8DEF;
    static final int PRIMARY_SOFT  = 0xFFE8F1FF;
    static final int RIPPLE        = 0x335B8DEF;
    static final int TEXT          = 0xFF2C3138;
    static final int SUBTEXT       = 0xFF7C8694;
    static final int DANGER_SOFT   = 0xFFFDE8EB;
    static final int DANGER        = 0xFFE0535F;
    static final int OK            = 0xFF3BB273;
    static final int OFF           = 0xFFC7CED9;
    static final int DIALOG_BG     = 0xFFF7F8FA;

    // 脚本/终端运行所需的完整环境
    static final String DEF_PATH =
        "/sbin:/system/sbin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/data/local/bin";
    static final String SCRIPT_ENV =
        "export PATH=" + DEF_PATH + " HOME=/data/local/tmp LANG=en_US.UTF-8 TERM=xterm";

    // ==================== 字段 ====================

    // 根容器
    private FrameLayout root;
    private TextView floatToast;

    // 主页（固定布局，不滚动）
    private FrameLayout homeContainer;
    private LinearLayout homeContent;
    private View statusDot;
    private TextView statusText, statusSub;
    private Button toggleBtn;
    private TextView targetPill, hidePill, scriptPill;

    // 运行日志（可滚动 + 可复制）
    private ScrollView logScroll;
    private TextView logView;
    private volatile boolean logAtBottom = true;
    private final StringBuilder logBuffer = new StringBuilder();

    // 应用选择器
    private LinearLayout pickerContent;
    private ScrollView pickerScroll;
    private TextView pickerTitle;
    private LinearLayout listBox;
    private EditText search;
    private Switch sysSwitch;
    private int mode = 0; // 0=目标应用 1=禁用应用

    // 脚本管理
    private LinearLayout scriptContent;
    private ScrollView scriptScroll;
    private LinearLayout scriptFileBox;
    private EditText scriptPathInput;
    private String scriptPath = "/sdcard";
    private File scriptPathPref;

    // 轻量终端
    private LinearLayout terminalContent;
    private ScrollView termScroll;
    private TextView termOut;
    private EditText termInput;
    private Button termSendBtn;
    private Process termProc = null;
    private BufferedWriter termIn = null;
    private String pendingRunScript = null;

    // 服务 & 配置
    private File cfgFile;
    private final ArrayList<AppInfo> allApps = new ArrayList<>();
    private volatile boolean rootGranted = false;
    private volatile boolean rootChecked = false;
    private volatile boolean serviceRunning = false;
    private Process serviceProc = null;

    // ==================== 内部类 ====================

    static class AppInfo {
        Drawable icon;
        String label;
        String pkg;
        boolean system;
        boolean checkedT, checkedF;

        AppInfo(Drawable icon, String label, String pkg, boolean system) {
            this.icon = icon;
            this.label = label;
            this.pkg = pkg;
            this.system = system;
        }
    }

    static class FsEntry {
        String name;
        boolean isDir;
        long size;

        FsEntry(String name, boolean isDir) {
            this.name = name;
            this.isDir = isDir;
        }
    }

    static class Res {
        int code;
        String out;

        Res(int code, String out) {
            this.code = code;
            this.out = out;
        }
    }

    // ==================== 生命周期 ====================

    @Override
    public void onCreate(Bundle b) {
        super.onCreate(b);
        cfgFile = new File(getFilesDir(), "config.txt");
        scriptPathPref = new File(getFilesDir(), "script_path.txt");
        loadScriptPath();
        buildUi();
        buildHome();
        ensureBinaryAsync();
        loadAppsAsync();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !rootChecked) {
            rootChecked = true;
            requestRoot();
        }
    }

    @Override
    public void onBackPressed() {
        if (pickerContent.getVisibility() == View.VISIBLE
                || scriptContent.getVisibility() == View.VISIBLE
                || terminalContent.getVisibility() == View.VISIBLE) {
            showHome();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (termProc != null) {
            try { termProc.destroy(); } catch (Exception ignored) {}
            termProc = null;
            termIn = null;
        }
    }

    // ==================== 工具方法 ====================

    private int dp(int n) {
        return (int) (n * getResources().getDisplayMetrics().density + 0.5f);
    }

    private GradientDrawable round(int color, float r) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(r);
        return g;
    }

    private Drawable glassRipple() {
        return new RippleDrawable(
            ColorStateList.valueOf(RIPPLE),
            getDrawable(R.drawable.bg_card),
            getDrawable(R.drawable.bg_card));
    }

    private Drawable tint(Drawable d, int color) {
        Drawable m = d.mutate();
        m.setTint(color);
        return m;
    }

    private Drawable icon(int res, int color) {
        return tint(getDrawable(res), color);
    }

    private TextView tv(String s, int size, int color, boolean bold) {
        TextView v = new TextView(this);
        v.setText(s);
        v.setTextSize(size);
        v.setTextColor(color);
        if (bold) v.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return v;
    }

    private String shq(String s) {
        return "'" + s.replace("'", "'\\''") + "'";
    }

    // ==================== UI 构建 ====================

    private void buildUi() {
        root = new FrameLayout(this);
        root.setBackground(getDrawable(R.drawable.bg_glass));
        applyInsets(root);

        // 悬浮胶囊提示
        floatToast = new TextView(this);
        floatToast.setTextColor(TEXT);
        floatToast.setTextSize(13);
        floatToast.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        GradientDrawable pill = new GradientDrawable();
        pill.setColor(0xEEFFFFFF);
        pill.setCornerRadius(dp(999));
        pill.setStroke(dp(1), 0x80FFFFFF);
        floatToast.setBackground(pill);
        floatToast.setElevation(dp(3));
        floatToast.setPadding(dp(20), dp(11), dp(20), dp(11));
        floatToast.setVisibility(View.GONE);
        floatToast.setAlpha(0f);
        FrameLayout.LayoutParams ftp = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL);
        ftp.bottomMargin = dp(96);
        root.addView(floatToast, ftp);

        // 主页容器（固定布局，禁止上下滑动）
        homeContainer = new FrameLayout(this);
        homeContent = new LinearLayout(this);
        homeContent.setOrientation(LinearLayout.VERTICAL);
        homeContent.setPadding(dp(20), dp(8), dp(20), dp(8));
        homeContainer.addView(homeContent, new FrameLayout.LayoutParams(-1, -1));
        root.addView(homeContainer, new FrameLayout.LayoutParams(-1, -1));

        // 应用选择器
        pickerContent = new LinearLayout(this);
        pickerContent.setOrientation(LinearLayout.VERTICAL);
        buildPicker();
        root.addView(pickerContent, new FrameLayout.LayoutParams(-1, -1));
        pickerContent.setVisibility(View.GONE);

        // 脚本管理
        scriptContent = new LinearLayout(this);
        scriptContent.setOrientation(LinearLayout.VERTICAL);
        buildScripts();
        root.addView(scriptContent, new FrameLayout.LayoutParams(-1, -1));
        scriptContent.setVisibility(View.GONE);

        // 轻量终端
        terminalContent = new LinearLayout(this);
        terminalContent.setOrientation(LinearLayout.VERTICAL);
        buildTerminal();
        root.addView(terminalContent, new FrameLayout.LayoutParams(-1, -1));
        terminalContent.setVisibility(View.GONE);

        setContentView(root);
    }

    private void applyInsets(final View v) {
        v.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override
            public android.view.WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                int top, bottom;
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets si = insets.getInsets(WindowInsets.Type.systemBars());
                    top = si.top;
                    bottom = si.bottom;
                } else {
                    top = insets.getSystemWindowInsetTop();
                    bottom = insets.getSystemWindowInsetBottom();
                }
                v.setPadding(0, top, 0, bottom);
                return insets;
            }
        });
    }

    // ==================== 主页 ====================

    private void buildHome() {
        homeContent.removeAllViews();

        // 状态卡片
        statusDot = new View(this);
        statusDot.setBackground(round(OFF, dp(8)));
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(dp(16), dp(16));
        dlp.setMargins(dp(4), 0, dp(12), 0);

        statusText = tv("服务未运行", 20, TEXT, true);
        statusSub = tv("检测中…", 13, SUBTEXT, false);
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(statusText);
        texts.addView(statusSub);

        LinearLayout statusCard = glassCard();
        statusCard.setGravity(Gravity.CENTER_VERTICAL);
        statusCard.setOrientation(LinearLayout.HORIZONTAL);
        statusCard.setPadding(dp(18), dp(20), dp(18), dp(20));
        LinearLayout.LayoutParams sclp = new LinearLayout.LayoutParams(-1, -2);
        sclp.setMargins(0, dp(10), 0, 0);
        homeContent.addView(statusCard, sclp);
        statusCard.addView(statusDot, dlp);
        LinearLayout.LayoutParams slp2 = new LinearLayout.LayoutParams(0, -2, 1f);
        statusCard.addView(texts, slp2);
        statusCard.setOnClickListener(v -> { if (!rootGranted) requestRoot(); });
        spring(statusCard);

        // 启动/停止切换按钮
        toggleBtn = new Button(this);
        toggleBtn.setAllCaps(false);
        toggleBtn.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        toggleBtn.setTextSize(15);
        toggleBtn.setElevation(dp(1));
        toggleBtn.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(-1, dp(52));
        tlp.setMargins(0, dp(10), 0, 0);
        toggleBtn.setLayoutParams(tlp);
        toggleBtn.setOnClickListener(v -> {
            if (serviceRunning) stopService(); else startService();
        });
        homeContent.addView(toggleBtn);
        spring(toggleBtn);
        updateToggleBtn();

        // 入口卡片
        targetPill = new TextView(this);
        homeContent.addView(menuCard("目标应用", "进入时触发禁用的应用",
            0, targetPill, 0xFF4A84E8, 0xFFE3EEFF, R.drawable.ic_target));
        hidePill = new TextView(this);
        homeContent.addView(menuCard("禁用应用", "进入目标后将被禁用的应用",
            1, hidePill, 0xFF8B6BE8, 0xFFF0E9FF, R.drawable.ic_snow));
        scriptPill = new TextView(this);
        homeContent.addView(menuCard("脚本管理", "轻量终端 · 系统环境执行脚本",
            2, scriptPill, 0xFF2FA66F, 0xFFE5F6EC, R.drawable.ic_terminal));

        // 运行日志卡片（weight=1 自适应剩余空间，保证底部按钮可见）
        LinearLayout logCard = glassCard();
        LinearLayout.LayoutParams lclp = new LinearLayout.LayoutParams(-1, 0, 1f);
        lclp.setMargins(0, dp(12), 0, 0);
        logCard.setPadding(dp(14), dp(12), dp(14), dp(12));
        homeContent.addView(logCard, lclp);

        LinearLayout logHeader = new LinearLayout(this);
        logHeader.setOrientation(LinearLayout.HORIZONTAL);
        logHeader.setGravity(Gravity.CENTER_VERTICAL);
        TextView logTitle = tv("运行日志", 15, TEXT, true);
        LinearLayout.LayoutParams ltlp = new LinearLayout.LayoutParams(0, -2, 1f);
        logHeader.addView(logTitle, ltlp);

        Button export = mkSmallBtn("导出", OK);
        LinearLayout.LayoutParams xlp = new LinearLayout.LayoutParams(-2, dp(34));
        xlp.setMargins(0, 0, dp(8), 0);
        export.setLayoutParams(xlp);
        logHeader.addView(export);

        Button clear = mkSmallBtn("清空", PRIMARY);
        logHeader.addView(clear);
        logCard.addView(logHeader);
        spring(export);
        spring(clear);
        export.setOnClickListener(v -> exportLog());
        clear.setOnClickListener(v -> clearLog());

        // 日志滚动区域（支持上下滑动 + 自由复制）
        logScroll = new ScrollView(this);
        logScroll.setVerticalScrollBarEnabled(true);
        logView = new TextView(this);
        logView.setTextSize(12);
        logView.setTextColor(SUBTEXT);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setPadding(dp(2), dp(8), dp(2), dp(4));
        logView.setTextIsSelectable(true);   // 允许自由复制
        logView.setLongClickable(true);
        logScroll.addView(logView);
        LinearLayout.LayoutParams lslp = new LinearLayout.LayoutParams(-1, 0, 1f);
        lslp.setMargins(0, dp(4), 0, 0);
        logCard.addView(logScroll, lslp);

        // 跟踪日志滚动位置
        logScroll.setOnScrollChangeListener((v, scrollX, scrollY, oldX, oldY) -> {
            View child = logScroll.getChildAt(0);
            logAtBottom = child == null || (scrollY + logScroll.getHeight()) >= child.getHeight() - 1;
        });

        // 底部链接栏（上移 + 重命名）
        LinearLayout bottomRow = new LinearLayout(this);
        bottomRow.setOrientation(LinearLayout.HORIZONTAL);
        bottomRow.setGravity(Gravity.CENTER);
        bottomRow.setPadding(0, dp(4), 0, dp(4));   // 从 dp(14) 减小，往上移
        homeContent.addView(bottomRow);

        TextView principle = new TextView(this);
        principle.setText("说明");                    // "工作原理" → "说明"
        principle.setTextSize(13);
        principle.setTextColor(PRIMARY);
        principle.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        principle.setPaintFlags(principle.getPaintFlags() | Paint.UNDERLINE_TEXT_FLAG);
        principle.setPadding(dp(10), dp(8), dp(10), dp(8));
        principle.setOnClickListener(v -> showPrinciple());
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(-2, -2);
        plp.setMargins(0, 0, dp(24), 0);
        bottomRow.addView(principle, plp);

        TextView bug = new TextView(this);
        bug.setText("Bug 反馈");
        bug.setTextSize(13);
        bug.setTextColor(PRIMARY);
        bug.setGravity(Gravity.CENTER);
        bug.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        bug.setPaintFlags(bug.getPaintFlags() | Paint.UNDERLINE_TEXT_FLAG);
        bug.setPadding(dp(10), dp(8), dp(10), dp(8));
        bug.setOnClickListener(v -> openBugReport());
        bottomRow.addView(bug);

        updateStatus();
    }

    // ==================== 公共 UI 组件 ====================

    private LinearLayout glassCard() {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setBackground(glassRipple());
        l.setElevation(dp(1));
        return l;
    }

    private Button mkSmallBtn(String s, int fg) {
        Button b = new Button(this);
        b.setText(s);
        b.setTextColor(fg);
        b.setTextSize(13);
        b.setAllCaps(false);
        b.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        b.setBackground(round(PRIMARY_SOFT, dp(12)));
        b.setPadding(dp(14), 0, dp(14), 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, dp(34));
        b.setLayoutParams(lp);
        return b;
    }

    private View menuCard(String title, String sub, final int m, TextView pill,
                          int fg, int bg, int ic) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        card.setBackground(glassRipple());
        card.setElevation(dp(1));
        LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(-1, -2);
        clp.setMargins(0, dp(8), 0, 0);
        card.setLayoutParams(clp);

        ImageView icon = new ImageView(this);
        icon.setImageDrawable(icon(ic, fg));
        icon.setBackground(round(bg, dp(14)));
        icon.setPadding(dp(11), dp(11), dp(11), dp(11));
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(dp(46), dp(46));
        ilp.setMargins(0, 0, dp(14), 0);
        card.addView(icon, ilp);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(tv(title, 17, TEXT, true));
        texts.addView(tv(sub, 13, SUBTEXT, false));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, -2, 1f);
        card.addView(texts, tlp);

        pill.setTextColor(PRIMARY);
        pill.setTextSize(12);
        pill.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        pill.setBackground(round(PRIMARY_SOFT, dp(12)));
        pill.setPadding(dp(10), dp(5), dp(10), dp(5));
        card.addView(pill);

        TextView chev = tv("\u203a", 22, SUBTEXT, false);
        chev.setPadding(dp(10), 0, 0, 0);
        card.addView(chev);

        card.setOnClickListener(v -> { if (m == 2) showScripts(); else showPicker(m); });
        spring(card);
        return card;
    }

    // 通用顶栏（返回键 + 标题）
    private LinearLayout topBar(String title, int backIcon, View.OnClickListener backListener) {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(6), dp(6), dp(16), dp(6));
        bar.setBackground(getDrawable(R.drawable.bg_topbar));

        ImageView back = new ImageView(this);
        back.setImageDrawable(icon(backIcon, TEXT));
        back.setPadding(dp(12), dp(8), dp(14), dp(8));
        bar.addView(back);
        back.setOnClickListener(backListener);

        TextView tv = tv(title, 20, TEXT, true);
        bar.addView(tv);
        return bar;
    }

    // ==================== 玻璃态弹窗（与主页 UI 风格一致） ====================

    /**
     * 创建与主页面 UI 一致的弹窗：玻璃态卡片背景、大圆角、半透明暗化
     */
    private Dialog makeGlassDialog(View content) {
        Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setContentView(content);
        Window w = dialog.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(round(DIALOG_BG, dp(24)));
            w.setDimAmount(0.4f);
            w.setLayout(dp(340), FrameLayout.LayoutParams.WRAP_CONTENT);
        }
        return dialog;
    }

    private Button dialogBtn(String text, int bgColor, int textColor) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(textColor);
        b.setTextSize(14);
        b.setAllCaps(false);
        b.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        b.setBackground(round(bgColor, dp(14)));
        b.setElevation(dp(1));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(44), 1f);
        b.setLayoutParams(lp);
        return b;
    }

    // ==================== 应用选择器 ====================

    private void buildPicker() {
        LinearLayout topbar = topBar("目标应用", R.drawable.ic_back, v -> showHome());
        pickerTitle = (TextView) topbar.getChildAt(1);
        pickerContent.addView(topbar);

        pickerScroll = new ScrollView(this);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16), dp(14), dp(16), dp(32));
        pickerScroll.addView(body);
        pickerContent.addView(pickerScroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        search = new EditText(this);
        search.setHint("搜索应用名称或包名");
        search.setTextSize(14);
        search.setHintTextColor(SUBTEXT);
        search.setTextColor(TEXT);
        search.setSingleLine(true);
        search.setBackground(getDrawable(R.drawable.bg_search));
        search.setPadding(dp(20), dp(12), dp(20), dp(12));
        search.setCompoundDrawablesWithIntrinsicBounds(
            icon(R.drawable.ic_search, SUBTEXT), null, null, null);
        search.setCompoundDrawablePadding(dp(10));
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) { rebuildList(); }
        });
        body.addView(search);

        LinearLayout sysRow = new LinearLayout(this);
        sysRow.setOrientation(LinearLayout.HORIZONTAL);
        sysRow.setGravity(Gravity.CENTER_VERTICAL);
        sysRow.setPadding(dp(2), dp(14), dp(2), dp(8));
        TextView sysLbl = tv("显示系统应用", 15, TEXT, false);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(0, -2, 1f);
        sysRow.addView(sysLbl, slp);
        sysSwitch = new Switch(this);
        sysSwitch.setChecked(false);
        sysSwitch.setOnCheckedChangeListener((v, is) -> rebuildList());
        sysRow.addView(sysSwitch);
        body.addView(sysRow);

        LinearLayout panel = glassCard();
        panel.setPadding(0, dp(6), 0, dp(6));
        listBox = new LinearLayout(this);
        listBox.setOrientation(LinearLayout.VERTICAL);
        panel.addView(listBox);
        body.addView(panel);
    }

    // ==================== 视图切换 ====================

    private void showHome() {
        pickerContent.setVisibility(View.GONE);
        scriptContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.GONE);
        homeContainer.setVisibility(View.VISIBLE);
        updateHomeCounts();
        updateStatus();
    }

    private void showPicker(int m) {
        mode = m;
        pickerTitle.setText(m == 0 ? "目标应用" : "禁用应用");
        homeContainer.setVisibility(View.GONE);
        scriptContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.GONE);
        pickerContent.setVisibility(View.VISIBLE);
        search.setText("");
        rebuildList();
    }

    private void showScripts() {
        homeContainer.setVisibility(View.GONE);
        pickerContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.GONE);
        scriptContent.setVisibility(View.VISIBLE);
        refreshFileList();
    }

    private void openTerminal(String runScript) {
        homeContainer.setVisibility(View.GONE);
        pickerContent.setVisibility(View.GONE);
        scriptContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.VISIBLE);
        if (runScript != null) {
            if (termProc != null) {
                termWrite(termScriptCmd(runScript));
            } else {
                pendingRunScript = runScript;
                termStart();
            }
        } else {
            termStart();
        }
        termInput.postDelayed(() -> {
            termInput.requestFocus();
            showKeyboard(termInput);
        }, 300);
    }

    // ==================== 应用列表 ====================

    private void loadAppsAsync() {
        new Thread(() -> {
            try {
                PackageManager pm = getPackageManager();
                ArrayList<ApplicationInfo> apps = new ArrayList<>(pm.getInstalledApplications(0));
                apps.sort((a, b) ->
                    pm.getApplicationLabel(a).toString().compareToIgnoreCase(
                        pm.getApplicationLabel(b).toString()));
                for (ApplicationInfo ai : apps) {
                    if (ai.packageName.equals(getPackageName())) continue;
                    String label = pm.getApplicationLabel(ai).toString();
                    Drawable icon = ai.loadIcon(pm);
                    boolean system = (ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
                    allApps.add(new AppInfo(icon, label, ai.packageName, system));
                }
                runOnUiThread(() -> {
                    applyConfigFromFile();
                    updateHomeCounts();
                    updateStatus();
                    appendLog("[系统] 已加载 " + allApps.size() + " 个应用");
                    if (pickerContent.getVisibility() == View.VISIBLE) rebuildList();
                });
            } catch (Exception e) {
                runOnUiThread(() ->
                    appendLog("[错误] 加载应用列表失败：" + e.getMessage()));
            }
        }).start();
    }

    private void applyConfigFromFile() {
        try {
            if (!cfgFile.exists()) return;
            HashSet<String> targets = new HashSet<>(), freezes = new HashSet<>();
            BufferedReader r = new BufferedReader(new FileReader(cfgFile));
            String line;
            while ((line = r.readLine()) != null) {
                String s = line.trim();
                if (s.startsWith("target:")) targets.add(s.substring(7).trim());
                else if (s.startsWith("freeze:")) freezes.add(s.substring(7).trim());
            }
            r.close();
            if (targets.isEmpty() && freezes.isEmpty()) return;
            for (AppInfo a : allApps) {
                a.checkedT = targets.contains(a.pkg);
                a.checkedF = freezes.contains(a.pkg);
                if (a.checkedT) a.checkedF = false;
            }
            appendLog("[系统] 已识别配置：目标 " + targets.size() + "，禁用 " + freezes.size());
        } catch (Exception ignored) {}
    }

    private void rebuildList() {
        listBox.removeAllViews();
        boolean showSys = sysSwitch.isChecked();
        String q = search.getText().toString().trim().toLowerCase();
        boolean hasQuery = q.length() > 0;
        ArrayList<AppInfo> list = new ArrayList<>();
        for (AppInfo a : allApps) {
            if (!showSys && a.system) continue;
            if (hasQuery && !(a.label.toLowerCase().contains(q)
                    || a.pkg.toLowerCase().contains(q))) continue;
            list.add(a);
        }
        // 已勾选的排最前
        final int m = mode;
        list.sort((a, b) -> {
            boolean ac = (m == 0) ? a.checkedT : a.checkedF;
            boolean bc = (m == 0) ? b.checkedT : b.checkedF;
            if (ac != bc) return ac ? -1 : 1;
            return 0;
        });
        for (AppInfo a : list) listBox.addView(appRow(a));
        if (list.isEmpty()) {
            TextView empty = tv(hasQuery ? "没有匹配的应用" : "未找到用户应用",
                14, SUBTEXT, false);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(0, dp(36), 0, dp(36));
            listBox.addView(empty);
        }
    }

    private View appRow(final AppInfo info) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(4), dp(7), dp(4), dp(7));
        row.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE), null, null));

        ImageView icon = new ImageView(this);
        icon.setImageDrawable(info.icon);
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(dp(40), dp(40));
        ilp.setMargins(0, 0, dp(12), 0);
        row.addView(icon, ilp);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(tv(info.label, 15, TEXT, true));
        texts.addView(tv(info.pkg, 12, SUBTEXT, false));
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, -2, 1f);
        row.addView(texts, tlp);

        final CheckBox cb = new CheckBox(this);
        cb.setButtonTintList(ColorStateList.valueOf(PRIMARY));
        if (mode == 0) cb.setChecked(info.checkedT);
        else cb.setChecked(info.checkedF);
        cb.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton v, boolean is) {
                if (is) {
                    if (mode == 0 && info.checkedF) {
                        info.checkedF = false;
                        appendLog("[配置] " + info.label + " 已从「禁用应用」移除（互斥）");
                    } else if (mode == 1 && info.checkedT) {
                        info.checkedT = false;
                        appendLog("[配置] " + info.label + " 已从「目标应用」移除（互斥）");
                    }
                }
                if (mode == 0) info.checkedT = is;
                else info.checkedF = is;
                updateHomeCounts();
                writeConfig();
                showFloat(info.label + (is ? " 已选择" : " 已取消"));
                listBox.post(() -> rebuildList());
            }
        });
        row.addView(cb);
        row.setOnClickListener(v -> cb.toggle());
        spring(row);
        return row;
    }

    private void updateHomeCounts() {
        int t = 0, f = 0;
        for (AppInfo a : allApps) {
            if (a.checkedT) t++;
            if (a.checkedF) f++;
        }
        if (targetPill != null) targetPill.setText("已选 " + t);
        if (hidePill != null) hidePill.setText("已选 " + f);
        if (scriptPill != null) scriptPill.setText("打开");
    }

    private void updateStatus() {
        int t = 0, f = 0;
        for (AppInfo a : allApps) {
            if (a.checkedT) t++;
            if (a.checkedF) f++;
        }
        statusText.setText(serviceRunning ? "服务运行中" : "服务未运行");
        statusDot.setBackground(round(serviceRunning ? OK : OFF, dp(8)));
        statusSub.setText((rootGranted ? "已获 root 权限" : "未获 root 权限（点击授权）")
            + "  ·  目标 " + t + " / 禁用 " + f);
        updateToggleBtn();
    }

    private void updateToggleBtn() {
        if (toggleBtn == null) return;
        if (serviceRunning) {
            toggleBtn.setText("守护服务正在运行中");
            toggleBtn.setTextColor(DANGER);
            toggleBtn.setBackground(round(DANGER_SOFT, dp(20)));
            toggleBtn.setElevation(dp(1));
        } else {
            toggleBtn.setText("启动服务");
            toggleBtn.setTextColor(0xFFFFFFFF);
            toggleBtn.setBackground(round(PRIMARY, dp(20)));
            toggleBtn.setElevation(dp(3));
        }
    }

    // ==================== 动效 ====================

    private void showFloat(final String msg) {
        if (floatToast == null) return;
        runOnUiThread(() -> {
            floatToast.setText(msg);
            floatToast.setVisibility(View.VISIBLE);
            floatToast.setAlpha(0f);
            floatToast.setScaleX(0.7f);
            floatToast.setScaleY(0.7f);
            floatToast.setTranslationY(dp(42));
            floatToast.animate().cancel();
            floatToast.animate()
                .alpha(1f).scaleX(1f).scaleY(1f).translationY(0)
                .setDuration(380).setInterpolator(new OvershootInterpolator(1.5f))
                .withEndAction(() -> floatToast.postDelayed(() ->
                    floatToast.animate().alpha(0f).translationY(-dp(18))
                        .setDuration(260).setStartDelay(900)
                        .withEndAction(() -> floatToast.setVisibility(View.GONE)).start(), 900))
                .start();
        });
    }

    private void spring(final View v) {
        v.setOnTouchListener((view, ev) -> {
            switch (ev.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    v.animate().cancel();
                    v.animate().scaleX(0.96f).scaleY(0.96f).setDuration(110).start();
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    v.animate().cancel();
                    v.animate().scaleX(1f).scaleY(1f).setDuration(300)
                        .setInterpolator(new OvershootInterpolator(2.2f)).start();
                    break;
            }
            return false;
        });
    }

    // ==================== Root / 服务 ====================

    private void requestRoot() {
        appendLog("[系统] 正在请求 root 权限…");
        new Thread(() -> {
            try {
                Res r = suExec("id");
                final boolean granted = r.code == 0;
                runOnUiThread(() -> {
                    rootGranted = granted;
                    if (granted) {
                        appendLog("[系统] 已授予 root 权限");
                        syncService();
                    } else {
                        appendLog("[系统] 未获得 root 权限");
                        promptNoRoot();
                    }
                    updateStatus();
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    appendLog("[系统] root 请求失败：" + e.getMessage());
                    promptNoRoot();
                });
            }
        }).start();
    }

    private void promptNoRoot() {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(22), dp(18), dp(22), dp(18));
        box.addView(tv("需要 Root 权限", 18, TEXT, true));
        TextView msg = tv("未检测到 root 权限。\n\n本应用需在已 root 的环境中运行，请先授予 root 权限（Magisk / KernelSU / APatch），再重新打开本应用。",
            14, SUBTEXT, false);
        msg.setLineSpacing(dp(2), 1f);
        msg.setPadding(0, dp(10), 0, dp(16));
        box.addView(msg);

        Button exitBtn = dialogBtn("退出", DANGER_SOFT, DANGER);
        box.addView(exitBtn);

        final Dialog dialog = makeGlassDialog(box);
        dialog.setCancelable(false);
        exitBtn.setOnClickListener(v -> { dialog.dismiss(); finish(); });
        dialog.show();
        spring(exitBtn);
    }

    private void syncService() {
        new Thread(() -> {
            try {
                boolean up = suExec("pgrep -x Guard").code == 0;
                runOnUiThread(() -> {
                    serviceRunning = up;
                    updateStatus();
                    if (up) appendLog("[系统] 检测到 Guard 服务已在运行");
                });
            } catch (Exception ignored) {}
        }).start();
    }

    private void startService() {
        if (!rootGranted) {
            appendLog("[错误] 尚未获得 root 权限，请先点击状态卡授权");
            updateStatus();
            return;
        }
        int tc = 0, fc = 0;
        for (AppInfo a : allApps) {
            if (a.checkedT) tc++;
            if (a.checkedF) fc++;
        }
        appendLog("[系统] 正在启动服务…（目标 " + tc + "，禁用 " + fc + "）");
        new Thread(() -> {
            try {
                if (suExec("pgrep -x Guard").code == 0) {
                    runOnUiThread(() -> {
                        serviceRunning = true;
                        updateStatus();
                        appendLog("[系统] Guard 服务已在运行，无需重复启动");
                        showFloat("服务已在运行");
                    });
                    return;
                }
                File exe = ensureBinary();
                String q = shq(exe.getAbsolutePath());
                String cfg = shq(cfgFile.getAbsolutePath());
                String cmd = "chmod 755 " + q + " ; " + q + " " + cfg;
                final Process p = new ProcessBuilder("su", "-c", cmd)
                    .redirectErrorStream(true).start();
                serviceProc = p;
                runOnUiThread(() -> {
                    serviceRunning = true;
                    updateStatus();
                    appendLog("[系统] 服务进程已启动，配置：" + cfgFile.getAbsolutePath());
                    showFloat("服务已启动");
                });
                BufferedReader r = new BufferedReader(
                    new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = r.readLine()) != null) {
                    // 去掉 Guard 自带的时间戳前缀
                    if (line.startsWith("[20")) {
                        int idx = line.indexOf("] ");
                        if (idx > 0) line = line.substring(idx + 2);
                    }
                    if (line.isEmpty()) continue;
                    appendLog(line);
                }
                runOnUiThread(() -> {
                    serviceRunning = false;
                    updateStatus();
                    appendLog("[系统] 服务已退出");
                });
                serviceProc = null;
            } catch (Exception e) {
                runOnUiThread(() ->
                    appendLog("[错误] 启动失败：" + e.getMessage()));
            }
        }).start();
    }

    private void stopService() {
        appendLog("[系统] 正在停止服务…");
        showFloat("服务已停止");
        runRoot("pkill -TERM -x Guard", "已发送停止信号");
    }

    // ==================== 配置 ====================

    private void writeConfig() {
        ArrayList<String> t = new ArrayList<>(), f = new ArrayList<>();
        for (AppInfo a : allApps) {
            if (a.checkedT) t.add(a.pkg);
            if (a.checkedF) f.add(a.pkg);
        }
        StringBuilder s = new StringBuilder();
        s.append("# Guard config\ninterval:2\n\n");
        for (String p : t) s.append("target:").append(p).append('\n');
        s.append('\n');
        for (String p : f) s.append("freeze:").append(p).append('\n');
        final String content = s.toString();
        try {
            try {
                writeConfigFile(cfgFile, content);
            } catch (Exception e) {
                if (cfgFile.exists() && !cfgFile.delete())
                    appendLog("[配置] 清理旧配置失败，改用 root 写入");
                writeConfigFile(cfgFile, content);
            }
            appendLog("[配置] 已保存：目标 " + t.size() + "，禁用 " + f.size()
                + " → " + cfgFile.getAbsolutePath());
        } catch (Exception e) {
            if (rootGranted) {
                try {
                    writeConfigAsRoot(content);
                    appendLog("[配置] 已保存(root)：目标 " + t.size() + "，禁用 " + f.size()
                        + " → " + cfgFile.getAbsolutePath());
                } catch (Exception e2) {
                    appendLog("[错误] 保存配置失败：" + e.getMessage());
                }
            } else {
                appendLog("[错误] 保存配置失败：" + e.getMessage());
            }
        }
    }

    private void writeConfigFile(File f, String content) throws IOException {
        FileWriter w = new FileWriter(f);
        w.write(content);
        w.close();
    }

    private void writeConfigAsRoot(String content) throws Exception {
        Process p = new ProcessBuilder("su", "-c",
            "cat > " + shq(cfgFile.getAbsolutePath()) +
            " && chmod 666 " + shq(cfgFile.getAbsolutePath())).start();
        OutputStream os = p.getOutputStream();
        os.write(content.getBytes("UTF-8"));
        os.flush();
        os.close();
        if (p.waitFor() != 0) throw new IOException("root 写入失败");
    }

    // ==================== 日志 ====================

    private void appendLog(final String line) {
        final String ts = new SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault())
            .format(new Date());
        runOnUiThread(() -> {
            logBuffer.append("[").append(ts).append("] ").append(line).append("\n");
            if (logBuffer.length() > 30000) logBuffer.delete(0, 15000);
            logView.setText(logBuffer.toString());
            if (logAtBottom) logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    private void clearLog() {
        logBuffer.setLength(0);
        logView.setText("");
    }

    private void exportLog() {
        final String content = logBuffer.toString();
        if (content.trim().isEmpty()) { showFloat("暂无日志可导出"); return; }
        final String name = "Guard日志_" + new SimpleDateFormat("yyyyMMdd_HHmmss",
            Locale.getDefault()).format(new Date()) + ".txt";
        final String path = "/sdcard/" + name;
        new Thread(() -> {
            try {
                if (rootGranted) {
                    File tmp = new File(getCacheDir(), "export_log.txt");
                    FileWriter tw = new FileWriter(tmp);
                    tw.write(content);
                    tw.close();
                    Res r = suExec("cp " + shq(tmp.getAbsolutePath()) + " "
                        + shq(path) + " && chmod 666 " + shq(path));
                    tmp.delete();
                    if (r.code == 0) { finishExport(path); return; }
                    Process p = new ProcessBuilder("su", "-c",
                        "cat > " + shq(path)).start();
                    OutputStream os = p.getOutputStream();
                    os.write(content.getBytes("UTF-8"));
                    os.flush();
                    os.close();
                    if (p.waitFor() == 0) { finishExport(path); return; }
                    throw new IOException("root 写入失败：" + r.out.trim());
                }
                FileWriter w = new FileWriter(path);
                w.write(content);
                w.close();
                finishExport(path);
            } catch (Exception e) {
                runOnUiThread(() -> {
                    appendLog("[错误] 导出日志失败：" + e.getMessage());
                    showFloat("导出失败");
                });
            }
        }).start();
    }

    private void finishExport(final String path) {
        runOnUiThread(() -> {
            appendLog("[系统] 日志已导出：" + path);
            showFloat("日志已导出到存储根目录");
        });
    }

    private void openBugReport() {
        try {
            Intent i = new Intent(Intent.ACTION_VIEW,
                Uri.parse("https://qm.qq.com/q/yox95mY2PY"));
            i.setPackage("com.tencent.mobileqq");
            if (i.resolveActivity(getPackageManager()) == null)
                i.setPackage(null);
            startActivity(i);
        } catch (Exception e) {
            appendLog("[错误] 打开反馈链接失败：" + e.getMessage());
        }
    }

    // ==================== 说明弹窗（玻璃态风格） ====================

    private View principleHeader(String s) {
        TextView v = tv(s, 15, PRIMARY, true);
        v.setPadding(0, dp(14), 0, dp(4));
        return v;
    }

    private View principleItem(String s) {
        TextView v = tv("· " + s, 14, TEXT, false);
        v.setPadding(dp(8), dp(3), dp(6), dp(3));
        v.setLineSpacing(dp(2), 1f);
        return v;
    }

    private void showPrinciple() {
        ScrollView sc = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(22), dp(14), dp(22), dp(18));
        sc.addView(box);

        box.addView(tv("说明", 20, TEXT, true));

        box.addView(principleHeader("一、核心思路"));
        box.addView(principleItem("两段式架构：应用负责配置、授权与部署，原生 Guard 进程专职「前台监听 + 系统级禁用」，职责清晰、互不干扰。"));
        box.addView(principleItem("另有「脚本管理 / 轻量终端」模块，支持以 root 权限运行 SH 脚本与编译后的二进制文件。"));
        box.addView(principleItem("通过系统事件日志感知前台应用变化，事件驱动、按需处理，空闲时进程开销趋近于零。"));

        box.addView(principleHeader("二、Guard 服务与禁用机制"));
        box.addView(principleItem("应用以 root 权限拉起 Guard，二者通过标准输出管道通信，运行日志实时回传界面；停止服务时自动恢复被修改过的应用。"));
        box.addView(principleItem("Guard 订阅系统事件日志（logcat -b events），仅过滤两类窗口事件：wm_on_resume_called 与 wm_on_top_resumed_gained_called，用 poll() 阻塞等待、事件驱动消费。"));
        box.addView(principleItem("前台组件名经前缀匹配识别，兼容纯包名、包名/类名、完整类名三种形态；命中目标即批量禁用（pm disable），离开即批量恢复（pm enable），全局状态标志保证操作幂等。"));
        box.addView(principleItem("每次禁用/恢复后回读应用状态校验，命令失败或状态未变会输出警告，确保执行结果可靠。"));

        box.addView(principleHeader("三、配置管理"));
        box.addView(principleItem("「目标应用」与「禁用应用」互斥，勾选时会自动从另一侧移除，避免自相矛盾。"));
        box.addView(principleItem("配置写入应用私有目录 config.txt，保存时自动修正文件权限，保证应用与 Guard 均可读写；root 写入失败时自动回退重试。"));
        box.addView(principleItem("Guard 每次启动时读取最新配置，修改后重启服务即可热加载，无需重新编译。"));

        box.addView(principleHeader("四、脚本管理 & 轻量终端"));
        box.addView(principleItem("通过 root 浏览系统各目录（含 /data 等受限路径），自动识别 ELF 二进制并直接执行，SH 脚本则按脚本运行，并注入完整环境变量。"));
        box.addView(principleItem("交互式运行窗口实时回显输出，底部输入框可随时向进程发送数字或内容，满足二进制程序的选择 / 输入需求。"));
        box.addView(principleItem("输出自动过滤 ANSI 颜色与转义码，避免乱码；轻量终端支持常驻 root shell、逐行输入与直接运行脚本。"));

        box.addView(principleHeader("五、进程清理与日志"));
        box.addView(principleItem("脚本执行结束后，按 cgroup 归属 + 执行链 PID 双重匹配，清理本次运行遗留的全部进程（包括但不限于 su、sh 及其子进程），并附 PID 与进程名写入日志。"));
        box.addView(principleItem("运行日志实时展示，支持一键导出到存储卡根目录（root 写入绕过分区存储限制），便于排查与反馈。"));

        Button okBtn = dialogBtn("已了解", PRIMARY, 0xFFFFFFFF);
        okBtn.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(44)));
        box.addView(okBtn);

        final Dialog dialog = makeGlassDialog(sc);
        okBtn.setOnClickListener(v -> dialog.dismiss());
        dialog.show();
        spring(okBtn);
    }

    // ==================== 脚本管理 ====================

    private void loadScriptPath() {
        try {
            if (scriptPathPref.exists()) {
                BufferedReader r = new BufferedReader(new FileReader(scriptPathPref));
                String s = r.readLine();
                r.close();
                if (s != null && !s.trim().isEmpty()) scriptPath = s.trim();
            }
        } catch (Exception ignored) {}
    }

    private void saveScriptPath() {
        try {
            FileWriter w = new FileWriter(scriptPathPref);
            w.write(scriptPath);
            w.close();
        } catch (Exception ignored) {}
    }

    private void buildScripts() {
        LinearLayout topbar = topBar("脚本管理", R.drawable.ic_back, v -> showHome());
        scriptContent.addView(topbar);

        scriptScroll = new ScrollView(this);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16), dp(14), dp(16), dp(32));
        scriptScroll.addView(body);
        scriptContent.addView(scriptScroll, new LinearLayout.LayoutParams(-1, 0, 1f));

        // 路径输入栏
        LinearLayout pathRow = new LinearLayout(this);
        pathRow.setOrientation(LinearLayout.HORIZONTAL);
        pathRow.setGravity(Gravity.CENTER_VERTICAL);
        scriptPathInput = new EditText(this);
        scriptPathInput.setHint("输入脚本目录路径，如 /sdcard/scripts");
        scriptPathInput.setTextSize(13);
        scriptPathInput.setHintTextColor(SUBTEXT);
        scriptPathInput.setTextColor(TEXT);
        scriptPathInput.setSingleLine(true);
        scriptPathInput.setTypeface(Typeface.MONOSPACE);
        scriptPathInput.setBackground(getDrawable(R.drawable.bg_search));
        scriptPathInput.setPadding(dp(14), dp(11), dp(14), dp(11));
        scriptPathInput.setImeOptions(EditorInfo.IME_ACTION_GO);
        scriptPathInput.setOnEditorActionListener((v, a, ev) -> {
            if (a == EditorInfo.IME_ACTION_GO || a == EditorInfo.IME_ACTION_DONE) {
                goScriptPath();
                return true;
            }
            return false;
        });
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(0, -2, 1f);
        pathRow.addView(scriptPathInput, plp);

        Button go = new Button(this);
        go.setText("打开");
        go.setTextColor(0xFFFFFFFF);
        go.setTextSize(13);
        go.setAllCaps(false);
        go.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        go.setBackground(round(0xFF2FA66F, dp(12)));
        LinearLayout.LayoutParams glp = new LinearLayout.LayoutParams(dp(80), dp(46));
        glp.setMargins(dp(10), 0, 0, 0);
        pathRow.addView(go, glp);
        body.addView(pathRow, new LinearLayout.LayoutParams(-1, -2));
        go.setOnClickListener(v -> goScriptPath());
        spring(go);

        TextView tip = tv("操作：输入脚本目录路径后点「打开」；点击文件夹逐级浏览；点击脚本文件即可运行（可勾选使用 SU 权限）。默认目录为上次保存的路径。",
            12, SUBTEXT, false);
        tip.setLineSpacing(dp(2), 1f);
        tip.setPadding(dp(2), dp(12), dp(2), dp(6));
        body.addView(tip);

        scriptFileBox = new LinearLayout(this);
        scriptFileBox.setOrientation(LinearLayout.VERTICAL);
        body.addView(scriptFileBox);
    }

    private void goScriptPath() {
        String p = scriptPathInput.getText().toString().trim();
        if (p.isEmpty()) { appendLog("[脚本] 请输入目录路径"); return; }
        scriptPath = p;
        saveScriptPath();
        refreshFileList();
    }

    private void refreshFileList() {
        scriptPathInput.setText(scriptPath);
        scriptFileBox.removeAllViews();
        File dir = new File(scriptPath);
        File parent = dir.getParentFile();

        // 上一级
        if (parent != null) {
            LinearLayout up = new LinearLayout(this);
            up.setOrientation(LinearLayout.HORIZONTAL);
            up.setGravity(Gravity.CENTER_VERTICAL);
            up.setPadding(dp(2), dp(9), dp(2), dp(9));
            up.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE), null, null));
            ImageView uic = new ImageView(this);
            uic.setImageDrawable(icon(R.drawable.ic_up, TEXT));
            uic.setPadding(dp(4), dp(4), dp(8), dp(4));
            up.addView(uic);
            up.addView(tv("上一级  " + parent.getAbsolutePath(), 14, TEXT, false));
            scriptFileBox.addView(up);
            up.setOnClickListener(v -> {
                scriptPath = parent.getAbsolutePath();
                saveScriptPath();
                refreshFileList();
            });
            spring(up);
        }

        // 列目录
        List<FsEntry> entries = new ArrayList<>();
        boolean javaOk = false;
        try {
            File[] fs = dir.listFiles();
            if (fs != null) {
                javaOk = true;
                for (File f : fs) {
                    FsEntry e = new FsEntry(f.getName(), f.isDirectory());
                    e.size = f.length();
                    entries.add(e);
                }
            }
        } catch (Exception ignored) {}

        if (!javaOk && rootGranted) entries = rootList(scriptPath);

        if (entries == null || entries.isEmpty()) {
            String msg = entries == null
                ? "无法读取该目录：无访问权限或路径无效"
                : (javaOk ? "目录为空" : "目录为空或无法读取（可尝试勾选 root 权限）");
            TextView e = tv(msg, 14, SUBTEXT, false);
            e.setGravity(Gravity.CENTER);
            e.setPadding(0, dp(30), 0, dp(30));
            scriptFileBox.addView(e);
            return;
        }

        Collections.sort(entries, (a, b) -> {
            if (a.isDir != b.isDir) return a.isDir ? -1 : 1;
            return a.name.compareToIgnoreCase(b.name);
        });

        for (final FsEntry e : entries) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(4), dp(8), dp(4), dp(8));
            row.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE), null, null));

            ImageView ic = new ImageView(this);
            ic.setImageDrawable(icon(e.isDir ? R.drawable.ic_folder : R.drawable.ic_file,
                e.isDir ? 0xFF4A84E8 : 0xFF2FA66F));
            LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(dp(34), dp(34));
            ilp.setMargins(0, 0, dp(12), 0);
            row.addView(ic, ilp);

            LinearLayout texts = new LinearLayout(this);
            texts.setOrientation(LinearLayout.VERTICAL);
            texts.addView(tv(e.name, 14, TEXT, true));
            String sub;
            if (e.isDir) sub = "文件夹";
            else sub = e.size > 0
                ? (e.size < 1024 ? e.size + " B" : String.format("%.1f KB", e.size / 1024.0))
                : "文件";
            texts.addView(tv(sub, 12, SUBTEXT, false));
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, -2, 1f);
            row.addView(texts, tlp);

            scriptFileBox.addView(row);
            final File target = new File(dir, e.name);
            row.setOnClickListener(v -> {
                if (e.isDir) {
                    scriptPath = target.getAbsolutePath();
                    saveScriptPath();
                    refreshFileList();
                } else {
                    runScriptDialog(target);
                }
            });
            spring(row);
        }
    }

    private List<FsEntry> rootList(String path) {
        List<FsEntry> out = new ArrayList<>();
        try {
            Res r = suExec("ls -Ap --color=never " + shq(path));
            if (r.code != 0) return null;
            if (r.out == null) return out;
            for (String line : r.out.split("\n")) {
                String s = line.trim();
                if (s.isEmpty() || s.equals(".") || s.equals("..")) continue;
                boolean isDir = s.endsWith("/");
                String name = isDir ? s.substring(0, s.length() - 1) : s;
                if (name.isEmpty()) continue;
                out.add(new FsEntry(name, isDir));
            }
            return out;
        } catch (Exception e) { return null; }
    }

    // ==================== 脚本运行弹窗（玻璃态风格） ====================

    private void runScriptDialog(final File f) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(22), dp(18), dp(22), dp(18));

        box.addView(tv("运行脚本 · " + f.getName(), 18, TEXT, true));

        TextView info = tv(f.getAbsolutePath(), 13, SUBTEXT, false);
        info.setTypeface(Typeface.MONOSPACE);
        info.setPadding(0, dp(8), 0, dp(8));
        box.addView(info);

        TextView hint = tv("运行时会弹出实时窗口：程序出现「请输入/选择」等提示时，在底部输入框输入数字或内容后点「发送」即可。",
            12, SUBTEXT, false);
        hint.setLineSpacing(dp(2), 1f);
        hint.setPadding(0, dp(4), 0, dp(12));
        box.addView(hint);

        final CheckBox su = new CheckBox(this);
        su.setText("使用 SU 权限运行（root）");
        su.setChecked(true);
        su.setTextColor(TEXT);
        su.setButtonTintList(ColorStateList.valueOf(0xFF2FA66F));
        box.addView(su);

        // 按钮行
        LinearLayout btnRow = new LinearLayout(this);
        btnRow.setOrientation(LinearLayout.HORIZONTAL);
        btnRow.setGravity(Gravity.CENTER);
        btnRow.setPadding(0, dp(16), 0, 0);

        Button cancelBtn = dialogBtn("取消", PRIMARY_SOFT, PRIMARY);
        Button runBtn = dialogBtn("运行", 0xFF2FA66F, 0xFFFFFFFF);
        LinearLayout.LayoutParams blp1 = new LinearLayout.LayoutParams(0, dp(44), 1f);
        blp1.setMargins(0, 0, dp(10), 0);
        cancelBtn.setLayoutParams(blp1);
        LinearLayout.LayoutParams blp2 = new LinearLayout.LayoutParams(0, dp(44), 1f);
        runBtn.setLayoutParams(blp2);
        btnRow.addView(cancelBtn);
        btnRow.addView(runBtn);
        box.addView(btnRow);

        final Dialog dialog = makeGlassDialog(box);
        cancelBtn.setOnClickListener(v -> dialog.dismiss());
        runBtn.setOnClickListener(v -> { dialog.dismiss(); runScript(f, su.isChecked()); });
        dialog.show();
        spring(cancelBtn);
        spring(runBtn);
    }

    // ==================== 脚本执行 ====================

    private void runScript(final File f, final boolean useSu) {
        final boolean binary = isElf(f);
        appendLog("[脚本] " + (useSu ? "SU" : "普通") + "运行：" + f.getAbsolutePath()
            + (binary ? "（检测为二进制可执行文件，直接执行）" : ""));

        // 交互式运行窗口（玻璃态风格）
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(18), dp(16), dp(18), dp(16));

        box.addView(tv("运行 · " + f.getName(), 18, TEXT, true));

        ScrollView sc = new ScrollView(this);
        sc.setVerticalScrollBarEnabled(true);
        final TextView out = new TextView(this);
        out.setTextSize(13);
        out.setTypeface(Typeface.MONOSPACE);
        out.setTextColor(TEXT);
        out.setLineSpacing(dp(2), 1f);
        out.setTextIsSelectable(true);
        out.setPadding(dp(12), dp(12), dp(12), dp(12));
        out.setBackground(round(0xFFF0F2F5, dp(12)));
        sc.addView(out);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(-1, dp(280));
        slp.setMargins(0, dp(12), 0, dp(12));
        box.addView(sc, slp);

        // 输入行
        LinearLayout inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setGravity(Gravity.CENTER_VERTICAL);
        final EditText input = new EditText(this);
        input.setHint("输入数字或内容，点「发送」");
        input.setTextSize(13);
        input.setHintTextColor(SUBTEXT);
        input.setTextColor(TEXT);
        input.setSingleLine(true);
        input.setTypeface(Typeface.MONOSPACE);
        input.setBackground(getDrawable(R.drawable.bg_search));
        input.setPadding(dp(12), dp(8), dp(12), dp(8));
        input.setImeOptions(EditorInfo.IME_ACTION_SEND);
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(0, -2, 1f);
        inputRow.addView(input, ilp);

        Button send = new Button(this);
        send.setText("发送");
        send.setTextColor(0xFFFFFFFF);
        send.setTextSize(13);
        send.setAllCaps(false);
        send.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        send.setBackground(round(0xFF2FA66F, dp(12)));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(dp(72), dp(42));
        blp.setMargins(dp(8), 0, 0, 0);
        inputRow.addView(send, blp);
        box.addView(inputRow);

        // 清理按钮（清理残留进程，保留用户二进制程序）
        final boolean preserveBinary = binary;
        Button doneBtn = dialogBtn("清理", PRIMARY_SOFT, PRIMARY);
        doneBtn.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(44)));
        ((LinearLayout.LayoutParams) doneBtn.getLayoutParams()).topMargin = dp(12);
        box.addView(doneBtn);

        final Process[] proc = { null };
        final boolean[] finished = { false };
        final long[] gotPid = { -1L };
        final long[] suPid = { -1L };
        final boolean[] userClosed = { false };
        final boolean[] userCleaned = { false };

        final Runnable sendInput = () -> {
            if (proc[0] == null || finished[0]) {
                liveAppend(out, sc, "\n[进程已结束，无法发送]");
                return;
            }
            String s = input.getText().toString();
            input.setText("");
            if (s.trim().isEmpty()) return;
            try {
                proc[0].getOutputStream().write((s + "\n").getBytes("UTF-8"));
                proc[0].getOutputStream().flush();
                liveAppend(out, sc, "> " + s);
            } catch (Exception e) {
                liveAppend(out, sc, "\n[发送失败：" + e.getMessage() + "]");
            }
        };
        send.setOnClickListener(v -> sendInput.run());
        input.setOnEditorActionListener((v, a, ev) -> {
            if (a == EditorInfo.IME_ACTION_SEND) { sendInput.run(); return true; }
            return false;
        });

        final Dialog dlg = makeGlassDialog(box);
        dlg.setCancelable(false);
        doneBtn.setOnClickListener(v -> {
            userCleaned[0] = true;
            // 立即输出日志（不等后台清理脚本跑完）
            appendLog("[脚本] 正在清理残留进程"
                + (preserveBinary ? "（保留用户程序）" : "") + "…");
            runOnUiThread(() -> {
                liveAppend(out, sc, "\n[正在清理残留进程…]");
            });
            // 清理残留进程（su、sh、终端等），但保留用户二进制程序
            new Thread(() -> {
                long su = suPid[0];
                long sh = gotPid[0] > 0 ? gotPid[0] : (useSu ? -1L : su);
                long preserve = -1;
                if (preserveBinary) {
                    preserve = gotPid[0] > 0 ? gotPid[0] : su;
                }
                cleanupLeftovers(su, sh, f.getName(), preserve);
            }).start();
            dlg.dismiss();
        });
        dlg.setOnDismissListener(d -> {
            userClosed[0] = true;
            // 如果是二进制程序且用户主动清理，不销毁进程（保留二进制）
            if (proc[0] != null && !(preserveBinary && userCleaned[0])) {
                try { proc[0].destroy(); } catch (Exception ignored) {}
            }
        });
        dlg.show();
        spring(doneBtn);
        spring(send);
        input.requestFocus();
        input.postDelayed(() -> showKeyboard(input), 200);

        new Thread(() -> {
            Process p = null;
            BufferedReader r = null;
            try {
                String dir = (f.getParentFile() != null)
                    ? f.getParentFile().getAbsolutePath() : "/";
                String path = shq(f.getAbsolutePath());
                String dirq = shq(dir);
                if (useSu) {
                    String run =
                        "echo __GUARD_PID__=$$; " +
                        "magic=$(head -c4 " + path + " 2>/dev/null | od -An -tx1 | tr -d ' \\n'); " +
                        "if [ \"$magic\" = \"7f454c46\" ]; then " +
                            "cd " + dirq + " && chmod +x " + path +
                            " && export LD_LIBRARY_PATH=" + dirq + ":\\\"$LD_LIBRARY_PATH\\\"" +
                            " && exec " + path + "; " +
                        "else " +
                            "cd " + dirq + " && " + SCRIPT_ENV +
                            " && exec /system/bin/sh " + path + "; " +
                        "fi";
                    p = new ProcessBuilder("su", "-c", run)
                        .redirectErrorStream(true).start();
                } else {
                    ProcessBuilder pb;
                    if (binary) {
                        f.setExecutable(true, false);
                        pb = new ProcessBuilder(f.getAbsolutePath());
                    } else {
                        pb = new ProcessBuilder("/system/bin/sh", f.getAbsolutePath());
                    }
                    pb.directory(f.getParentFile());
                    Map<String, String> e = pb.environment();
                    e.put("PATH", DEF_PATH);
                    e.put("HOME", "/data/local/tmp");
                    e.put("LANG", "en_US.UTF-8");
                    e.put("TERM", "xterm");
                    p = pb.redirectErrorStream(true).start();
                }
                proc[0] = p;
                suPid[0] = pidOf(p);
                r = new BufferedReader(new InputStreamReader(p.getInputStream(), "UTF-8"));
                String line;
                while ((line = r.readLine()) != null) {
                    if (line.startsWith("__GUARD_PID__=")) {
                        try {
                            gotPid[0] = Long.parseLong(
                                line.substring("__GUARD_PID__=".length()).trim());
                        } catch (Exception ignored) {}
                        continue;
                    }
                    final String s = line;
                    runOnUiThread(() -> liveAppend(out, sc, s));
                }
                int code = p.waitFor();
                final int rc = code;
                runOnUiThread(() -> {
                    finished[0] = true;
                    liveAppend(out, sc, "\n[执行完毕，退出码 " + rc + "]");
                    appendLog("[脚本] 执行完毕，退出码 " + rc);
                });
            } catch (Exception e) {
                final String msg = e.getMessage();
                final boolean cancelled = userClosed[0];
                runOnUiThread(() -> {
                    finished[0] = true;
                    liveAppend(out, sc, "\n[" + (cancelled
                        ? "已取消（进程被关闭）"
                        : ("执行失败：" + msg)) + "]");
                    if (!cancelled) appendLog("[脚本] 执行失败：" + msg);
                });
            } finally {
                boolean hadProcess = p != null;
                if (r != null) { try { r.close(); } catch (Exception ignored) {} }
                if (p != null) {
                    try { p.getOutputStream().close(); } catch (Exception ignored) {}
                    try { p.getInputStream().close(); } catch (Exception ignored) {}
                    // 二进制程序且用户已清理：不销毁进程（保留二进制）
                    if (!(preserveBinary && userCleaned[0])) {
                        try { p.destroy(); } catch (Exception ignored) {}
                    }
                }
                proc[0] = null;
                // 用户已点「清理」则跳过（避免重复清理导致延迟）
                if (hadProcess && !userCleaned[0]) {
                    long sh = gotPid[0] > 0 ? gotPid[0]
                        : (useSu ? -1L : suPid[0]);
                    cleanupLeftovers(suPid[0], sh, f.getName(), -1);
                }
            }
        }).start();
    }

    private void liveAppend(final TextView out, final ScrollView sc, final String s) {
        String t = cleanAnsi(s);
        if (t.isEmpty()) return;
        out.append(t + "\n");
        sc.post(() -> sc.fullScroll(View.FOCUS_DOWN));
    }

    private String cleanAnsi(String s) {
        if (s == null) return "";
        String t = s.replaceAll("\u001B\\[[0-9;?]*[ -/]*[@-~]", "")
                 .replaceAll("\u001B\\][^\\u0007\\u001B]*(\\u0007|\\u001B\\\\)", "");
        int end = t.length();
        while (end > 0 && t.charAt(end - 1) == '\r') end--;
        return t.substring(0, end);
    }

    private boolean isElf(File f) {
        try {
            FileInputStream in = new FileInputStream(f);
            byte[] b = new byte[4];
            int n = in.read(b);
            in.close();
            return n == 4 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
        } catch (Exception e) { return false; }
    }

    private String termScriptCmd(String path) {
        String p = shq(path);
        String dir = shq(new File(path).getParent() != null
            ? new File(path).getParent() : "/");
        return "if [ \"$(head -c4 " + p + " 2>/dev/null | od -An -tx1 | tr -d ' \\n')\"" +
               " = \"7f454c46\" ]; then " +
               "cd " + dir + " && chmod +x " + p +
               " && export LD_LIBRARY_PATH=" + dir + ":\\\"$LD_LIBRARY_PATH\\\"" +
               " && exec " + p + "; " +
               "else cd " + dir + " && sh " + p + "; fi";
    }

    private void showScriptResult(String name, int code, String out) {
        ScrollView sc = new ScrollView(this);
        TextView tv = new TextView(this);
        tv.setText("退出码：" + code + "\n\n"
            + (out == null || out.isEmpty() ? "（无输出）" : out));
        tv.setTextSize(13);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextColor(TEXT);
        tv.setTextIsSelectable(true);
        tv.setPadding(dp(18), dp(12), dp(18), dp(12));
        sc.addView(tv);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(22), dp(18), dp(22), dp(18));
        box.addView(tv(name, 18, TEXT, true));
        box.addView(sc, new LinearLayout.LayoutParams(-1, dp(300)));

        Button closeBtn = dialogBtn("关闭", PRIMARY_SOFT, PRIMARY);
        closeBtn.setLayoutParams(new LinearLayout.LayoutParams(-1, dp(44)));
        ((LinearLayout.LayoutParams) closeBtn.getLayoutParams()).topMargin = dp(12);
        box.addView(closeBtn);

        final Dialog dialog = makeGlassDialog(box);
        closeBtn.setOnClickListener(v -> dialog.dismiss());
        dialog.show();
        spring(closeBtn);
    }

    // ==================== 轻量终端 ====================

    private void buildTerminal() {
        LinearLayout topbar = topBar("脚本终端", R.drawable.ic_back, v -> showHome());
        // 清屏按钮
        LinearLayout.LayoutParams tsp = new LinearLayout.LayoutParams(0, -2, 1f);
        topbar.addView(new View(this), tsp);
        Button clear = mkSmallBtn("清屏", SUBTEXT);
        topbar.addView(clear);
        terminalContent.addView(topbar);
        clear.setOnClickListener(v -> termOut.setText(""));

        termScroll = new ScrollView(this);
        termScroll.setVerticalScrollBarEnabled(true);
        termOut = new TextView(this);
        termOut.setTextSize(13);
        termOut.setTextColor(0xFFD8F0D8);
        termOut.setTypeface(Typeface.MONOSPACE);
        termOut.setLineSpacing(dp(2), 1f);
        termOut.setPadding(dp(12), dp(12), dp(12), dp(12));
        termOut.setBackground(round(0xFF10141C, dp(16)));
        termOut.setTextIsSelectable(true);    // 终端输出支持复制
        termOut.setLongClickable(true);
        termScroll.addView(termOut);
        LinearLayout.LayoutParams tl = new LinearLayout.LayoutParams(-1, 0, 1f);
        tl.setMargins(dp(14), dp(12), dp(14), dp(4));
        terminalContent.addView(termScroll, tl);

        LinearLayout inputRow = new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setGravity(Gravity.CENTER_VERTICAL);
        inputRow.setPadding(dp(14), dp(6), dp(14), dp(14));
        termInput = new EditText(this);
        termInput.setHint("输入命令，回车执行");
        termInput.setTextSize(14);
        termInput.setHintTextColor(0xFF7C8694);
        termInput.setTextColor(0xFF2C3138);
        termInput.setSingleLine(true);
        termInput.setTypeface(Typeface.MONOSPACE);
        termInput.setBackground(getDrawable(R.drawable.bg_search));
        termInput.setPadding(dp(16), dp(10), dp(16), dp(10));
        termInput.setImeOptions(EditorInfo.IME_ACTION_SEND);
        termInput.setOnEditorActionListener((v, a, ev) -> {
            if (a == EditorInfo.IME_ACTION_SEND) { termSend(); return true; }
            return false;
        });
        LinearLayout.LayoutParams ilp = new LinearLayout.LayoutParams(0, -2, 1f);
        inputRow.addView(termInput, ilp);
        termSendBtn = new Button(this);
        termSendBtn.setText("发送");
        termSendBtn.setTextColor(0xFFFFFFFF);
        termSendBtn.setTextSize(14);
        termSendBtn.setAllCaps(false);
        termSendBtn.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        termSendBtn.setBackground(round(0xFF2FA66F, dp(14)));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(dp(76), dp(44));
        blp.setMargins(dp(10), 0, 0, 0);
        inputRow.addView(termSendBtn, blp);
        terminalContent.addView(inputRow);
        termSendBtn.setOnClickListener(v -> termSend());
        spring(termSendBtn);
    }

    private void showKeyboard(View v) {
        try {
            InputMethodManager im = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            im.showSoftInput(v, InputMethodManager.SHOW_IMPLICIT);
        } catch (Exception ignored) {}
    }

    private void termStart() {
        if (termProc != null) return;
        appendTerm("$ 正在启动终端…\n");
        new Thread(() -> {
            try {
                ProcessBuilder pb;
                if (rootGranted) {
                    pb = new ProcessBuilder("su", "-c", SCRIPT_ENV + " && sh");
                } else {
                    pb = new ProcessBuilder("/system/bin/sh");
                    Map<String, String> e = pb.environment();
                    e.put("PATH", DEF_PATH);
                    e.put("HOME", "/data/local/tmp");
                    e.put("LANG", "en_US.UTF-8");
                    e.put("TERM", "xterm");
                }
                pb.redirectErrorStream(true);
                final Process p = pb.start();
                termProc = p;
                termIn = new BufferedWriter(new OutputStreamWriter(p.getOutputStream()));
                appendTerm(rootGranted
                    ? "$ root@android:/ # 就绪（root shell），可直接输入命令\n"
                    : "$ app@android:/ $ 就绪（普通 shell）\n");
                final String run = pendingRunScript;
                pendingRunScript = null;
                if (run != null) termInput.postDelayed(
                    () -> termWrite(termScriptCmd(run)), 400);
                BufferedReader r = new BufferedReader(
                    new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = r.readLine()) != null) {
                    final String s = line;
                    runOnUiThread(() -> appendTerm(s + "\n"));
                }
                termProc = null;
                termIn = null;
                runOnUiThread(() -> appendTerm("$ 终端已退出\n"));
            } catch (Exception e) {
                runOnUiThread(() ->
                    appendTerm("[错误] 启动终端失败：" + e.getMessage() + "\n"));
                termProc = null;
                termIn = null;
            }
        }).start();
    }

    private void appendTerm(final String s) {
        runOnUiThread(() -> {
            termOut.append(cleanAnsi(s));
            termScroll.post(() -> termScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    private void termWrite(String cmd) {
        if (termProc == null || termIn == null) {
            appendTerm("[错误] 终端未运行\n");
            return;
        }
        appendTerm("$ " + cmd + "\n");
        try { termIn.write(cmd + "\n"); termIn.flush(); }
        catch (Exception e) { appendTerm("[错误] 写入失败：" + e.getMessage() + "\n"); }
    }

    private void termSend() {
        String cmd = termInput.getText().toString();
        termInput.setText("");
        if (cmd.trim().isEmpty()) return;
        termWrite(cmd);
    }

    // ==================== 工具 ====================

    private void ensureBinaryAsync() {
        new Thread(() -> {
            try { ensureBinary(); } catch (Exception ignored) {}
        }).start();
    }

    private File ensureBinary() throws IOException {
        File f = new File(getFilesDir(), EXE_NAME);
        InputStream in = getAssets().open(ASSET);
        OutputStream os = new FileOutputStream(f);
        byte[] buf = new byte[16384];
        int n;
        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        os.close();
        in.close();
        f.setExecutable(true, false);
        return f;
    }

    // 获取进程 PID
    static long pidOf(Process p) {
        if (p == null) return -1L;
        try { return p.pid(); } catch (Throwable ignored) {}
        try {
            java.lang.reflect.Field f = p.getClass().getDeclaredField("pid");
            f.setAccessible(true);
            try { return f.getLong(p); }
            catch (IllegalArgumentException e) { return f.getInt(p); }
        } catch (Throwable ignored) {}
        return -1L;
    }

    // 清理脚本遗留进程（preservePid 对应的进程不会被杀）
    private void cleanupLeftovers(final long suPid, final long shPid,
                                  final String scriptName, final long preservePid) {
        try {
            if (!rootGranted) {
                appendLog("[脚本] 已清理所有遗留进程（无 root，跳过）");
                return;
            }
            final StringBuilder report = new StringBuilder();
            final String appPid = String.valueOf(android.os.Process.myPid());
            final StringBuilder cmd = new StringBuilder();

            // === Phase 1: 快速杀（进程组/pgrep，0.1 秒杀 95%）===
            if (preservePid > 0) {
                // 保留二进制：pgrep -P 杀 su/sh 的直接子进程，跳过 preserve
                final String P = String.valueOf(preservePid);
                final String A = appPid;
                if (suPid > 0) {
                    cmd.append("for c in $(pgrep -P ").append(suPid)
                       .append(" 2>/dev/null); do [ \"$c\" = \"").append(P)
                       .append("\" ] || [ \"$c\" = \"").append(A)
                       .append("\" ] && continue; kill -9 \"$c\" 2>/dev/null; done; ");
                    cmd.append("kill -9 ").append(suPid).append(" 2>/dev/null; ");
                    report.append(" PID=").append(suPid).append("（su）");
                }
                if (shPid > 0 && shPid != preservePid && shPid != suPid) {
                    cmd.append("for c in $(pgrep -P ").append(shPid)
                       .append(" 2>/dev/null); do [ \"$c\" = \"").append(P)
                       .append("\" ] || [ \"$c\" = \"").append(A)
                       .append("\" ] && continue; kill -9 \"$c\" 2>/dev/null; done; ");
                    cmd.append("kill -9 ").append(shPid).append(" 2>/dev/null; ");
                    String lbl = (scriptName != null && !scriptName.isEmpty())
                        ? scriptName : "sh";
                    report.append(" PID=").append(shPid).append("（").append(lbl).append("）");
                }
            } else {
                // 不保留：进程组杀 + 直杀已知 PID
                if (shPid > 0) {
                    cmd.append("kill -9 -").append(shPid).append(" 2>/dev/null; ");
                    String lbl = (scriptName != null && !scriptName.isEmpty())
                        ? scriptName : "sh";
                    report.append(" PID=").append(shPid).append("（").append(lbl).append("）");
                }
                if (suPid > 0) {
                    cmd.append("kill -9 ").append(suPid).append(" 2>/dev/null; ");
                    report.append(" PID=").append(suPid).append("（su）");
                }
                if (shPid > 0) {
                    cmd.append("kill -9 ").append(shPid).append(" 2>/dev/null; ");
                }
            }

            // === Phase 2: cgroup 扫描补杀逃逸进程（setsid/nohup/管道/daemon，一轮 SIGKILL 无 sleep）===
            final StringBuilder skip = new StringBuilder();
            skip.append(appPid).append(' ');
            if (preservePid > 0) skip.append(preservePid).append(' ');
            if (suPid > 0) skip.append(suPid).append(' ');
            if (shPid > 0) skip.append(shPid).append(' ');
            cmd.append("CG=$(cat /proc/self/cgroup 2>/dev/null")
               .append(" | awk -F: 'NF>1{print $NF}' | sort -r | head -1); ");
            cmd.append("[ -z \"$CG\" ] && CG=__NO_CG__; ");
            cmd.append("UI=$(echo \"$CG\" | grep -o '/uid_[0-9]*' | head -1); ");
            cmd.append("for p in /proc/[0-9]*; do ");
            cmd.append("pid=${p#/proc/}; ");
            cmd.append("[ \"$pid\" = \"").append(appPid).append("\" ] && continue; ");
            cmd.append("[ \"$pid\" = \"$$\" ] && continue; ");
            cmd.append("[ \"$pid\" = \"$PPID\" ] && continue; ");
            cmd.append("case \" ").append(skip).append("\" in *\" $pid \"*) continue;; esac; ");
            cmd.append("cg=$(cat \"$p/cgroup\" 2>/dev/null) || continue; ");
            // 匹配同一 cgroup 或同一 uid 段
            cmd.append("case \"$cg\" in \"$CG\"|\"$CG\"/*) kill -9 \"$pid\" 2>/dev/null; continue;; esac; ");
            cmd.append("[ -n \"$UI\" ] && case \"$cg\" in *\"$UI\"*) kill -9 \"$pid\" 2>/dev/null;; esac; ");
            cmd.append("done");

            suExec(cmd.toString());

            if (report.length() == 0)
                appendLog("[脚本] 已清理所有遗留进程（无残留）");
            else
                appendLog("[脚本] 已清理所有遗留进程" + report);
        } catch (Exception e) {
            appendLog("[脚本] 清理遗留进程失败：" + e.getMessage());
        }
    }

    private Res suExec(String cmd) throws Exception {
        Process p = new ProcessBuilder("su", "-c", cmd)
            .redirectErrorStream(true).start();
        String out = read(p.getInputStream());
        int code = p.waitFor();
        return new Res(code, out);
    }

    private void runRoot(String command, String okMsg) {
        new Thread(() -> {
            try {
                Res r = suExec(command);
                appendLog(r.code == 0 ? ("[系统] " + okMsg)
                    : ("操作失败：" + r.out.trim()));
            } catch (Exception e) {
                appendLog("Root 执行失败：" + e.getMessage());
            }
        }).start();
    }

    static String read(InputStream in) throws Exception {
        BufferedReader r = new BufferedReader(new InputStreamReader(in));
        StringBuilder s = new StringBuilder();
        String x;
        while ((x = r.readLine()) != null) s.append(x).append('\n');
        return s.toString();
    }
}
