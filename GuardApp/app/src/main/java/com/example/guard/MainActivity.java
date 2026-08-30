package com.example.guard;

import android.app.Activity;
import android.app.AlertDialog;
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

public class MainActivity extends Activity {
    static final String ASSET="guard_arm64-v8a";
    static final String EXE_NAME="guard";

    static final int PRIMARY=0xFF5B8DEF, PRIMARY_SOFT=0xFFE8F1FF, RIPPLE=0x335B8DEF,
        TEXT=0xFF2C3138, SUBTEXT=0xFF7C8694, DANGER_SOFT=0xFFFDE8EB, DANGER=0xFFE0535F,
        OK=0xFF3BB273, OFF=0xFFC7CED9;
    // 脚本/终端运行所需的完整环境：补全 PATH 与常用变量（默认 PATH 不完整，导致脚本命令找不到）
    // GUARD_TASK=1 用于 C 守护进程遍历 /proc/<pid>/environ 时精准识别脚本血统进程，触发目标应用时一键清理
    static final String DEF_PATH="/sbin:/system/sbin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin:/data/local/bin";
    static final String SCRIPT_ENV="export PATH="+DEF_PATH+" HOME=/data/local/tmp LANG=en_US.UTF-8 TERM=xterm GUARD_TASK=1";

    TextView statusText, statusSub, targetPill, hidePill, scriptPill, pickerTitle, logView, floatToast;
    View statusDot;
    Button toggleBtn;
    FrameLayout root;
    ScrollView homeScroll, pickerScroll, logScroll;
    LinearLayout homeContent, pickerContent, listBox;
    EditText search;
    Switch sysSwitch;
    int mode=0; // 0=目标应用 1=禁用应用
    File cfgFile;
    final ArrayList<AppInfo> allApps=new ArrayList<>();

    volatile boolean rootGranted=false, rootChecked=false, serviceRunning=false;
    volatile boolean logAtBottom=true;
    Process serviceProc=null;
    final Object logLock=new Object();
    final StringBuilder logBuffer=new StringBuilder();
    Thread logTailerThread;
    volatile boolean logTailerRunning;
    long guardLogOffset;
    File uiLogFile(){ return new File(getFilesDir(),"ui.log"); }
    File guardLogFile(){ return new File(getFilesDir(),"guard.log"); }

    // ===== 脚本终端 & 脚本管理 =====
    LinearLayout scriptContent, terminalContent, scriptFileBox;
    ScrollView scriptScroll, termScroll;
    TextView termOut;
    EditText termInput, scriptPathInput;
    Button termSendBtn;
    Process termProc=null;
    BufferedWriter termIn=null;
    String pendingRunScript=null;
    String scriptPath="/sdcard";
    File scriptPathPref;

    static class AppInfo {
        Drawable icon; String label; String pkg; boolean system;
        boolean checkedT, checkedF;
        AppInfo(Drawable icon,String label,String pkg,boolean system){
            this.icon=icon; this.label=label; this.pkg=pkg; this.system=system;
        }
    }

    static class FsEntry {
        String name; boolean isDir; long size;
        FsEntry(String name,boolean isDir){ this.name=name; this.isDir=isDir; }
    }

    @Override public void onCreate(Bundle b){
        super.onCreate(b);
        cfgFile=new File(getFilesDir(),"config.txt");
        scriptPathPref=new File(getFilesDir(),"script_path.txt");
        loadScriptPath();
        buildUi();
        buildHome();
        // 日志三件套：载入历史 + 启动守护日志实时追踪（确保 App UI 被杀后也能在重开时看到全过程）
        loadHistoricLogs();
        startLogTailer();
        ensureBinaryAsync();
        loadAppsAsync();
    }

    @Override public void onWindowFocusChanged(boolean hasFocus){
        super.onWindowFocusChanged(hasFocus);
        if(hasFocus&&!rootChecked){
            rootChecked=true;
            requestRoot();
        }
    }

    int dp(int n){ return (int)(n*getResources().getDisplayMetrics().density+0.5f); }
    GradientDrawable round(int color,float r){ GradientDrawable g=new GradientDrawable(); g.setColor(color); g.setCornerRadius(r); return g; }
    Drawable glassRipple(){ return new RippleDrawable(ColorStateList.valueOf(RIPPLE), getDrawable(R.drawable.bg_card), getDrawable(R.drawable.bg_card)); }
    Drawable tint(Drawable d,int color){ Drawable m=d.mutate(); m.setTint(color); return m; }
    Drawable icon(int res,int color){ return tint(getDrawable(res),color); }

    TextView tv(String s,int size,int color,boolean bold){
        TextView v=new TextView(this); v.setText(s); v.setTextSize(size); v.setTextColor(color);
        if(bold) v.setTypeface(Typeface.DEFAULT,Typeface.BOLD); return v;
    }

    // AlertDialog 自定义内容统一容器：固定 VERTICAL + 传入 dp padding，避免各弹窗贴边程度不一
    LinearLayout dialogBox(int padDp){
        LinearLayout l=new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(dp(padDp),dp(padDp),dp(padDp),dp(padDp));
        return l;
    }

    // ==================== UI ====================
    void buildUi(){
        root=new FrameLayout(this);
        root.setBackground(getDrawable(R.drawable.bg_glass));
        applyInsets(root);

        // Apple 风格悬浮胶囊提示（浅色毛玻璃，与主 UI 一致）
        floatToast=new TextView(this);
        floatToast.setTextColor(TEXT);
        floatToast.setTextSize(13);
        floatToast.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        GradientDrawable pill=new GradientDrawable();
        pill.setColor(0xEEFFFFFF);
        pill.setCornerRadius(dp(999));
        pill.setStroke(dp(1),0x80FFFFFF);
        floatToast.setBackground(pill);
        floatToast.setElevation(dp(3));
        floatToast.setPadding(dp(20),dp(11),dp(20),dp(11));
        floatToast.setVisibility(View.GONE);
        floatToast.setAlpha(0f);
        FrameLayout.LayoutParams ftp=new FrameLayout.LayoutParams(-2,-2,Gravity.BOTTOM|Gravity.CENTER_HORIZONTAL);
        ftp.bottomMargin=dp(96);
        root.addView(floatToast,ftp);

        homeScroll=new ScrollView(this);
        homeContent=new LinearLayout(this); homeContent.setOrientation(LinearLayout.VERTICAL);
        homeContent.setPadding(dp(20),dp(12),dp(20),dp(32));
        homeScroll.addView(homeContent); root.addView(homeScroll);

        pickerContent=new LinearLayout(this); pickerContent.setOrientation(LinearLayout.VERTICAL);
        buildPicker();
        root.addView(pickerContent,new FrameLayout.LayoutParams(-1,-1)); pickerContent.setVisibility(View.GONE);

        scriptContent=new LinearLayout(this); scriptContent.setOrientation(LinearLayout.VERTICAL);
        buildScripts();
        root.addView(scriptContent,new FrameLayout.LayoutParams(-1,-1)); scriptContent.setVisibility(View.GONE);

        terminalContent=new LinearLayout(this); terminalContent.setOrientation(LinearLayout.VERTICAL);
        buildTerminal();
        root.addView(terminalContent,new FrameLayout.LayoutParams(-1,-1)); terminalContent.setVisibility(View.GONE);
        setContentView(root);
    }

    void applyInsets(final View v){
        v.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener(){
            @Override public android.view.WindowInsets onApplyWindowInsets(View view, WindowInsets insets){
                int top,bottom;
                if(Build.VERSION.SDK_INT>=30){
                    android.graphics.Insets si=insets.getInsets(WindowInsets.Type.systemBars());
                    top=si.top; bottom=si.bottom;
                }else{
                    top=insets.getSystemWindowInsetTop();
                    bottom=insets.getSystemWindowInsetBottom();
                }
                v.setPadding(0,top,0,bottom);
                return insets;
            }
        });
    }

    void buildHome(){
        homeContent.removeAllViews();

        // ===== 中间：服务运行状态卡 =====
        statusDot=new View(this);
        statusDot.setBackground(round(OFF,dp(8)));
        LinearLayout.LayoutParams dlp=new LinearLayout.LayoutParams(dp(16),dp(16));
        dlp.setMargins(dp(4),0,dp(12),0);

        statusText=tv("服务未运行",20,TEXT,true);
        statusSub=tv("检测中…",13,SUBTEXT,false);
        LinearLayout texts=new LinearLayout(this); texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(statusText); texts.addView(statusSub);

        LinearLayout statusCard=glassCard();
        statusCard.setGravity(Gravity.CENTER_VERTICAL);
        statusCard.setOrientation(LinearLayout.HORIZONTAL);
        statusCard.setPadding(dp(18),dp(20),dp(18),dp(20));
        LinearLayout.LayoutParams sclp=new LinearLayout.LayoutParams(-1,-2);
        sclp.setMargins(0,dp(18),0,0);
        homeContent.addView(statusCard,sclp);
        statusCard.addView(statusDot,dlp);
        LinearLayout.LayoutParams slp2=new LinearLayout.LayoutParams(0,-2,1f);
        statusCard.addView(texts,slp2);
        statusCard.setOnClickListener(v->{ if(!rootGranted) requestRoot(); });
        spring(statusCard);

        // 启动/停止合并为一个切换按钮
        toggleBtn=new Button(this);
        toggleBtn.setAllCaps(false); toggleBtn.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        toggleBtn.setTextSize(15); toggleBtn.setElevation(dp(1));
        toggleBtn.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams tlp=new LinearLayout.LayoutParams(-1,dp(52));
        tlp.setMargins(0,dp(18),0,0);
        toggleBtn.setLayoutParams(tlp);
        toggleBtn.setOnClickListener(v->{
            if(serviceRunning) stopService(); else startService();
        });
        homeContent.addView(toggleBtn);
        spring(toggleBtn);
        updateToggleBtn();

        // 两个入口卡片
        homeContent.addView(menuCard("目标应用","进入时触发禁用的应用",0,targetPill=new TextView(this),0xFF4A84E8,0xFFE3EEFF,R.drawable.ic_target));
        homeContent.addView(menuCard("禁用应用","进入目标后将被禁用的应用",1,hidePill=new TextView(this),0xFF8B6BE8,0xFFF0E9FF,R.drawable.ic_snow));
        homeContent.addView(menuCard("脚本管理","轻量终端 · 系统环境执行脚本",2,scriptPill=new TextView(this),0xFF2FA66F,0xFFE5F6EC,R.drawable.ic_terminal));

        // ===== 底部：运行日志框 =====
        LinearLayout logCard=glassCard();
        LinearLayout.LayoutParams lclp=new LinearLayout.LayoutParams(-1,-2);
        lclp.setMargins(0,dp(16),0,0);
        logCard.setPadding(dp(14),dp(12),dp(14),dp(12));
        homeContent.addView(logCard,lclp);

        LinearLayout logHeader=new LinearLayout(this);
        logHeader.setOrientation(LinearLayout.HORIZONTAL);
        logHeader.setGravity(Gravity.CENTER_VERTICAL);
        TextView logTitle=tv("运行日志",15,TEXT,true);
        LinearLayout.LayoutParams ltlp=new LinearLayout.LayoutParams(0,-2,1f);
        logHeader.addView(logTitle,ltlp);
        Button export=mkSmallBtn("导出",OK);
        LinearLayout.LayoutParams xlp=new LinearLayout.LayoutParams(-2,dp(34));
        xlp.setMargins(0,0,dp(8),0);
        export.setLayoutParams(xlp);
        logHeader.addView(export);
        Button clear=mkSmallBtn("清空",PRIMARY);
        logHeader.addView(clear);
        logCard.addView(logHeader);
        spring(export);
        export.setOnClickListener(v->exportLog());
        spring(clear);
        clear.setOnClickListener(v->clearLog());

        logScroll=new ScrollView(this);
        logScroll.setVerticalScrollBarEnabled(true);
        logView=new TextView(this);
        logView.setTextSize(12); logView.setTextColor(SUBTEXT);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setPadding(dp(2),dp(8),dp(2),dp(4));
        logScroll.addView(logView);
        LinearLayout.LayoutParams lslp=new LinearLayout.LayoutParams(-1,dp(190));
        lslp.setMargins(0,dp(4),0,0);
        logCard.addView(logScroll,lslp);
        // 修复与主页外层 ScrollView 的滚动冲突：日志框内触摸时，禁止外层拦截，使内部可独立滚动
        logScroll.setOnTouchListener((v,e)->{
            switch(e.getActionMasked()){
                case MotionEvent.ACTION_DOWN:
                    homeScroll.requestDisallowInterceptTouchEvent(true);
                    break;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    homeScroll.requestDisallowInterceptTouchEvent(false);
                    break;
            }
            return false;
        });
        // 跟踪是否滚到底部：滚到底部时新日志自动跟随，向上翻阅时暂停跟随
        logScroll.setOnScrollChangeListener((v,scrollX,scrollY,oldX,oldY)->{
            View child=logScroll.getChildAt(0);
            logAtBottom=child==null||(scrollY+logScroll.getHeight())>=child.getHeight()-1;
        });

        // 底部：工作原理 + Bug 反馈
        LinearLayout bottomRow=new LinearLayout(this);
        bottomRow.setOrientation(LinearLayout.HORIZONTAL);
        bottomRow.setGravity(Gravity.CENTER);
        bottomRow.setPadding(0,dp(14),0,dp(8));
        homeContent.addView(bottomRow);

        TextView principle=new TextView(this);
        principle.setText("工作原理");
        principle.setTextSize(13);
        principle.setTextColor(PRIMARY);
        principle.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        principle.setPaintFlags(principle.getPaintFlags()|Paint.UNDERLINE_TEXT_FLAG);
        principle.setPadding(dp(10),dp(8),dp(10),dp(8));
        principle.setOnClickListener(v->showPrinciple());
        LinearLayout.LayoutParams plp=new LinearLayout.LayoutParams(-2,-2);
        plp.setMargins(0,0,dp(24),0);
        bottomRow.addView(principle,plp);

        TextView bug=new TextView(this);
        bug.setText("Bug 反馈");
        bug.setTextSize(13);
        bug.setTextColor(PRIMARY);
        bug.setGravity(Gravity.CENTER);
        bug.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        bug.setPaintFlags(bug.getPaintFlags()|Paint.UNDERLINE_TEXT_FLAG);
        bug.setPadding(dp(10),dp(8),dp(10),dp(8));
        bug.setOnClickListener(v->openBugReport());
        bottomRow.addView(bug);

        updateStatus();
    }

    LinearLayout glassCard(){
        LinearLayout l=new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setBackground(glassRipple());
        l.setElevation(dp(1));
        return l;
    }

    Button mkSmallBtn(String s,int fg){
        Button b=new Button(this);
        b.setText(s); b.setTextColor(fg); b.setTextSize(13);
        b.setAllCaps(false); b.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        b.setBackground(round(PRIMARY_SOFT,dp(12)));
        b.setPadding(dp(14),0,dp(14),0);
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(-2,dp(34));
        b.setLayoutParams(lp);
        return b;
    }

    View menuCard(String title,String sub,final int m,TextView pill,int fg,int bg,int ic){
        LinearLayout card=new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(16),dp(14),dp(16),dp(14));
        card.setBackground(glassRipple()); card.setElevation(dp(1));
        LinearLayout.LayoutParams clp=new LinearLayout.LayoutParams(-1,-2);
        clp.setMargins(0,dp(12),0,0);
        card.setLayoutParams(clp);

        ImageView icon=new ImageView(this);
        icon.setImageDrawable(icon(ic,fg));
        icon.setBackground(round(bg,dp(14)));
        icon.setPadding(dp(11),dp(11),dp(11),dp(11));
        LinearLayout.LayoutParams ilp=new LinearLayout.LayoutParams(dp(46),dp(46));
        ilp.setMargins(0,0,dp(14),0);
        card.addView(icon,ilp);

        LinearLayout texts=new LinearLayout(this); texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(tv(title,17,TEXT,true));
        texts.addView(tv(sub,13,SUBTEXT,false));
        LinearLayout.LayoutParams tlp=new LinearLayout.LayoutParams(0,-2,1f);
        card.addView(texts,tlp);

        pill.setTextColor(PRIMARY); pill.setTextSize(12); pill.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        pill.setBackground(round(PRIMARY_SOFT,dp(12))); pill.setPadding(dp(10),dp(5),dp(10),dp(5));
        card.addView(pill);
        TextView chev=tv("›",22,SUBTEXT,false); chev.setPadding(dp(10),0,0,0);
        card.addView(chev);

        card.setOnClickListener(v->{ if(m==2) showScripts(); else showPicker(m); });
        spring(card);
        return card;
    }

    void buildPicker(){
        LinearLayout topbar=new LinearLayout(this);
        topbar.setOrientation(LinearLayout.HORIZONTAL);
        topbar.setGravity(Gravity.CENTER_VERTICAL);
        topbar.setPadding(dp(6),dp(6),dp(16),dp(6));
        topbar.setBackground(getDrawable(R.drawable.bg_topbar));

        ImageView back=new ImageView(this);
        back.setImageDrawable(icon(R.drawable.ic_back,TEXT));
        back.setPadding(dp(12),dp(8),dp(14),dp(8));
        topbar.addView(back);
        pickerTitle=tv("目标应用",20,TEXT,true);
        topbar.addView(pickerTitle);
        pickerContent.addView(topbar);
        back.setOnClickListener(v->showHome());

        pickerScroll=new ScrollView(this);
        LinearLayout body=new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16),dp(14),dp(16),dp(32));
        pickerScroll.addView(body);
        pickerContent.addView(pickerScroll,new LinearLayout.LayoutParams(-1,0,1f));

        search=new EditText(this);
        search.setHint("搜索应用名称或包名"); search.setTextSize(14);
        search.setHintTextColor(SUBTEXT); search.setTextColor(TEXT);
        search.setSingleLine(true);
        search.setBackground(getDrawable(R.drawable.bg_search));
        search.setPadding(dp(20),dp(12),dp(20),dp(12));
        search.setCompoundDrawablesWithIntrinsicBounds(icon(R.drawable.ic_search,SUBTEXT),null,null,null);
        search.setCompoundDrawablePadding(dp(10));
        search.addTextChangedListener(new TextWatcher(){
            @Override public void beforeTextChanged(CharSequence s,int a,int b,int c){}
            @Override public void onTextChanged(CharSequence s,int a,int b,int c){ rebuildList(); }
            @Override public void afterTextChanged(Editable s){}
        });
        body.addView(search);

        LinearLayout sysRow=new LinearLayout(this);
        sysRow.setOrientation(LinearLayout.HORIZONTAL);
        sysRow.setGravity(Gravity.CENTER_VERTICAL);
        sysRow.setPadding(dp(2),dp(14),dp(2),dp(8));
        TextView sysLbl=tv("显示系统应用",15,TEXT,false);
        LinearLayout.LayoutParams slp=new LinearLayout.LayoutParams(0,-2,1f);
        sysRow.addView(sysLbl,slp);
        sysSwitch=new Switch(this); sysSwitch.setChecked(false);
        sysSwitch.setOnCheckedChangeListener((v,is)->rebuildList());
        sysRow.addView(sysSwitch);
        body.addView(sysRow);

        LinearLayout panel=glassCard();
        panel.setPadding(0,dp(6),0,dp(6));
        listBox=new LinearLayout(this); listBox.setOrientation(LinearLayout.VERTICAL);
        panel.addView(listBox);
        body.addView(panel);
    }

    // ==================== 导航 ====================
    void showHome(){
        pickerContent.setVisibility(View.GONE);
        scriptContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.GONE);
        homeScroll.setVisibility(View.VISIBLE);
        updateHomeCounts();
        updateStatus();
    }
    void showPicker(int m){
        mode=m;
        pickerTitle.setText(m==0?"目标应用":"禁用应用");
        homeScroll.setVisibility(View.GONE);
        scriptContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.GONE);
        pickerContent.setVisibility(View.VISIBLE);
        search.setText("");
        rebuildList();
    }
    void showScripts(){
        homeScroll.setVisibility(View.GONE);
        pickerContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.GONE);
        scriptContent.setVisibility(View.VISIBLE);
        refreshFileList();
    }
    void openTerminal(String runScript){
        homeScroll.setVisibility(View.GONE);
        pickerContent.setVisibility(View.GONE);
        scriptContent.setVisibility(View.GONE);
        terminalContent.setVisibility(View.VISIBLE);
        if(runScript!=null){
            if(termProc!=null){ termWrite(termScriptCmd(runScript)); }
            else{ pendingRunScript=runScript; termStart(); }
        }else{
            termStart();
        }
        termInput.postDelayed(()->{ termInput.requestFocus(); showKeyboard(termInput); }, 300);
    }

    // 构建在终端内自动运行的命令：shell 内判定 ELF（二进制直接执行，否则按 sh 脚本运行），并 cd 到所在目录
    String termScriptCmd(String path){
        String p=shq(path);
        String dir=shq(new File(path).getParent()!=null?new File(path).getParent():"/");
        return "if [ \"$(head -c4 "+p+" 2>/dev/null | od -An -tx1 | tr -d ' \\n')\" = \"7f454c46\" ]; then "+
               "cd "+dir+" && chmod +x "+p+" && export LD_LIBRARY_PATH="+dir+":\\\"$LD_LIBRARY_PATH\\\" && exec "+p+"; "+
               "else cd "+dir+" && sh "+p+"; fi";
    }
    @Override public void onBackPressed(){
        if(scriptContent.getVisibility()==View.VISIBLE) showHome();
        else if(terminalContent.getVisibility()==View.VISIBLE) showHome();
        else if(pickerContent.getVisibility()==View.VISIBLE) showHome();
        else super.onBackPressed();
    }

    // ==================== 数据 ====================
    void loadAppsAsync(){
        new Thread(()->{
            try{
                PackageManager pm=getPackageManager();
                ArrayList<ApplicationInfo> apps=new ArrayList<>(pm.getInstalledApplications(0));
                apps.sort((a,b)->pm.getApplicationLabel(a).toString().compareToIgnoreCase(pm.getApplicationLabel(b).toString()));
                for(ApplicationInfo ai:apps){
                    // Guard 自身允许加入列表（可作为「禁用应用」隐藏自身），后续在 UI 层屏蔽「目标应用」模式勾选
                    String label=pm.getApplicationLabel(ai).toString();
                    Drawable icon=ai.loadIcon(pm);
                    boolean system=(ai.flags&ApplicationInfo.FLAG_SYSTEM)!=0;
                    allApps.add(new AppInfo(icon,label,ai.packageName,system));
                }
                runOnUiThread(()->{
                    applyConfigFromFile();
                    updateHomeCounts();
                    updateStatus();
                    appendLog("[系统] 已加载 "+allApps.size()+" 个应用");
                    if(pickerContent.getVisibility()==View.VISIBLE) rebuildList();
                });
            }catch(Exception e){
                runOnUiThread(()->appendLog("[错误] 加载应用列表失败："+e.getMessage()));
            }
        }).start();
    }

    void applyConfigFromFile(){
        try{
            if(!cfgFile.exists()) return;
            HashSet<String> targets=new HashSet<>(), freezes=new HashSet<>();
            BufferedReader r=new BufferedReader(new FileReader(cfgFile));
            String line;
            while((line=r.readLine())!=null){
                String s=line.trim();
                if(s.startsWith("target:")) targets.add(s.substring(7).trim());
                else if(s.startsWith("freeze:")) freezes.add(s.substring(7).trim());
            }
            r.close();
            // —— 互斥防线（文件加载阶段）：同一 pkg 同时在两边时，目标优先，从 freezes 中剔除 ——
            int overlap=0;
            for(String p:new HashSet<>(freezes)){
                if(targets.contains(p)){ freezes.remove(p); overlap++; }
            }
            // Guard 自身不允许作为触发目标（作为前台应用触发逻辑无意义）
            if(targets.remove(getPackageName())) overlap++;
            if(targets.isEmpty()&&freezes.isEmpty()) return;
            for(AppInfo a: allApps){
                a.checkedT=targets.contains(a.pkg);
                a.checkedF=freezes.contains(a.pkg);
            }
            String overlapHint=overlap>0? "（冲突 "+overlap+" 条已按规则清理）":"";
            appendLog("[系统] 已识别配置：目标 "+targets.size()+"，禁用 "+freezes.size()+overlapHint);
        }catch(Exception ignored){}
    }

    void rebuildList(){
        listBox.removeAllViews();
        boolean showSys=sysSwitch.isChecked();
        String q=search.getText().toString().trim().toLowerCase();
        boolean hasQuery=q.length()>0;
        ArrayList<AppInfo> list=new ArrayList<>();
        for(AppInfo a: allApps){
            if(!showSys&&a.system) continue;
            if(hasQuery&&!(a.label.toLowerCase().contains(q)||a.pkg.toLowerCase().contains(q))) continue;
            list.add(a);
        }
        // 已勾选的应用自动排到最前，其余保持字母序（稳定排序）
        final int m=mode;
        list.sort((a,b)->{
            boolean ac=(m==0)?a.checkedT:a.checkedF;
            boolean bc=(m==0)?b.checkedT:b.checkedF;
            if(ac!=bc) return ac?-1:1;
            return 0;
        });
        for(AppInfo a:list) listBox.addView(appRow(a));
        if(list.isEmpty()){
            TextView empty=tv(hasQuery?"没有匹配的应用":"未找到用户应用",14,SUBTEXT,false);
            empty.setGravity(Gravity.CENTER); empty.setPadding(0,dp(36),0,dp(36));
            listBox.addView(empty);
        }
    }

    View appRow(final AppInfo info){
        LinearLayout row=new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(4),dp(7),dp(4),dp(7));
        row.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE),null,null));

        ImageView icon=new ImageView(this);
        icon.setImageDrawable(info.icon);
        LinearLayout.LayoutParams ilp=new LinearLayout.LayoutParams(dp(40),dp(40));
        ilp.setMargins(0,0,dp(12),0);
        row.addView(icon,ilp);

        LinearLayout texts=new LinearLayout(this); texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(tv(info.label,15,TEXT,true));
        texts.addView(tv(info.pkg,12,SUBTEXT,false));
        LinearLayout.LayoutParams tlp=new LinearLayout.LayoutParams(0,-2,1f);
        row.addView(texts,tlp);

        final CheckBox cb=new CheckBox(this);
        cb.setButtonTintList(ColorStateList.valueOf(PRIMARY));
        if(mode==0) cb.setChecked(info.checkedT); else cb.setChecked(info.checkedF);

        final boolean isSelfPkg=info.pkg.equals(getPackageName());
        if(isSelfPkg && mode==0){
            // 本应用不可作为「目标应用」触发源，禁用勾选 + 灰色提示
            cb.setEnabled(false);
            texts.addView(tv("（本应用不可作为触发目标，可加入「禁用应用」隐藏自身）",11,0xFF99A4B6,false));
        }

        cb.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener(){
            @Override public void onCheckedChanged(CompoundButton v,boolean is){
                //  Guard 自身 + 目标模式：不允许任何操作（上面已禁用 CheckBox，这里再兜底）
                if(is && isSelfPkg && mode==0){
                    v.setChecked(false);
                    showFloat(info.label+" 不可作为触发目标");
                    return;
                }
                if(is){
                    // 互斥：同一应用不能同时是「目标应用」和「禁用应用」—— 新勾选的一侧优先，另一侧自动移除
                    if(mode==0&&info.checkedF){
                        info.checkedF=false;
                        appendLog("[配置] "+info.label+" 已自动从「禁用应用」移除（与目标应用互斥）");
                    }else if(mode==1&&info.checkedT){
                        info.checkedT=false;
                        appendLog("[配置] "+info.label+" 已自动从「目标应用」移除（与禁用应用互斥）");
                    }
                }
                if(mode==0) info.checkedT=is; else info.checkedF=is;
                updateHomeCounts();
                writeConfig(); // 勾选即自动保存
                showFloat(info.label+(is?" 已选择":" 已取消"));
                listBox.post(()->rebuildList()); // 勾选后自动置顶
            }
        });
        row.addView(cb);
        row.setOnClickListener(v->cb.toggle());
        spring(row);
        return row;
    }

    void updateHomeCounts(){
        int t=0,f=0;
        for(AppInfo a: allApps){ if(a.checkedT)t++; if(a.checkedF)f++; }
        if(targetPill!=null) targetPill.setText("已选 "+t);
        if(hidePill!=null) hidePill.setText("已选 "+f);
        if(scriptPill!=null) scriptPill.setText("打开");
    }

    void updateStatus(){
        int t=0,f=0;
        for(AppInfo a: allApps){ if(a.checkedT)t++; if(a.checkedF)f++; }
        statusText.setText(serviceRunning?"服务运行中":"服务未运行");
        statusDot.setBackground(round(serviceRunning?OK:OFF,dp(8)));
        statusSub.setText((rootGranted?"已获 root 权限":"未获 root 权限（点击授权）")
            +"  ·  目标 "+t+" / 禁用 "+f);
        updateToggleBtn();
    }

    void updateToggleBtn(){
        if(toggleBtn==null) return;
        if(serviceRunning){
            toggleBtn.setText("守护服务正在运行中");
            toggleBtn.setTextColor(DANGER);
            toggleBtn.setBackground(round(DANGER_SOFT,dp(20)));
            toggleBtn.setElevation(dp(1));
        }else{
            toggleBtn.setText("启动服务");
            toggleBtn.setTextColor(0xFFFFFFFF);
            toggleBtn.setBackground(round(PRIMARY,dp(20)));
            toggleBtn.setElevation(dp(3));
        }
    }

    // ==================== Apple 风格动效 ====================
    // 悬浮胶囊提示：从底部弹性弹出、停留后淡出（类似 iOS toast）
    void showFloat(final String msg){
        if(floatToast==null) return;
        runOnUiThread(()->{
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
                .withEndAction(()->floatToast.postDelayed(()->
                    floatToast.animate().alpha(0f).translationY(-dp(18))
                        .setDuration(260).setStartDelay(900)
                        .withEndAction(()->floatToast.setVisibility(View.GONE)).start(), 900))
                .start();
        });
    }

    // 按压弹性动画：按下缩小、松开回弹（iOS 风格 spring）
    void spring(final View v){
        v.setOnTouchListener((view,ev)->{
            switch(ev.getActionMasked()){
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
    void requestRoot(){
        appendLog("[系统] 正在请求 root 权限…");
        new Thread(()->{
            try{
                Res r=suExec("id");
                final boolean granted=r.code==0;
                runOnUiThread(()->{
                    rootGranted=granted;
                    if(granted){
                        appendLog("[系统] 已授予 root 权限");
                        syncService();
                    }else{
                        appendLog("[系统] 未获得 root 权限");
                        promptNoRoot();
                    }
                    updateStatus();
                });
            }catch(Exception e){
                runOnUiThread(()->{
                    appendLog("[系统] root 请求失败："+e.getMessage());
                    promptNoRoot();
                });
            }
        }).start();
    }

    void promptNoRoot(){
        new AlertDialog.Builder(this)
            .setTitle("需要 Root 权限")
            .setMessage("未检测到 root 权限。\n\n本应用需在已 root 的环境中运行，请先授予 root 权限（Magisk / KernelSU / APatch），再重新打开本应用。")
            .setCancelable(false)
            .setPositiveButton("退出",(d,w)->finish())
            .show();
    }

    // ============= Guard 存活精准判定（pidfile + kill -0 + Name 三重锁）=============
    // 替代原先 pgrep -x Guard，避免僵尸/同名/su 返回码异常造成的“显示在运行实际没跑/停不掉”
    static final String GUARD_NAME="Guard";

    File pidFile(){ return new File(cfgFile.getAbsolutePath()+".pid"); }

    int readPidFile(){
        try{
            File f=pidFile();
            if(!f.exists()||f.length()==0) return 0;
            String s=new String(java.nio.file.Files.readAllBytes(f.toPath()),"UTF-8").trim();
            int pid=Integer.parseInt(s);
            return pid>1 ? pid : 0;
        }catch(Exception ignored){ return 0; }
    }

    boolean aliveByPid(int pid) throws Exception {
        if(pid<=1) return false;
        // 双重认证：1) kill -0 进程存在；2) /proc/<pid>/status Name == Guard（防 PID 复用）
        String check="kill -0 "+pid+" 2>/dev/null && { Name=$(cat /proc/"+pid+"/status 2>/dev/null | grep '^Name:' | awk '{print $2}'); [ \"$Name\" = \""+GUARD_NAME+"\" ] && exit 0; } || exit 1";
        return suExec(check).code==0;
    }

    int findGuardPid() throws Exception {
        // 1) 优先 pidfile（最新写入、最精准）
        int fp=readPidFile();
        if(fp>1 && aliveByPid(fp)) return fp;

        // 2) 兜底 pgrep -x，再核对 /proc 名字（避免 pgrep 命中原僵尸/同名进程）
        Res r=suExec("pgrep -x "+GUARD_NAME+" | head -n 1");
        if(r.code!=0) return 0;
        String[] lines=r.out.trim().split("\\s+");
        for(String s:lines){
            try{
                int pid=Integer.parseInt(s.trim());
                if(aliveByPid(pid)) return pid;
            }catch(Exception ignored){}
        }
        // 清理无效 pidfile
        File pf=pidFile(); if(pf.exists()) pf.delete();
        return 0;
    }

    boolean isRunningReal(){
        try{ return findGuardPid()>1; }
        catch(Exception e){ return false; }
    }

    void syncService(){
        new Thread(()->{
            try{
                final int pid=findGuardPid();
                final boolean up=pid>1;
                runOnUiThread(()->{
                    serviceRunning=up;
                    updateStatus();
                    if(up) appendLog("[系统] 检测到 Guard 服务已在运行（pid="+pid+"）");
                });
            }catch(Exception ignored){}
        }).start();
    }

    void startService(){
        if(!rootGranted){ appendLog("[错误] 尚未获得 root 权限，请先点击状态卡授权"); updateStatus(); return; }
        int tc=0, fc=0;
        for(AppInfo a: allApps){ if(a.checkedT)tc++; if(a.checkedF)fc++; }
        appendLog("[系统] 正在启动服务…（目标 "+tc+"，禁用 "+fc+"）");
        new Thread(()->{
            try{
                int oldPid=findGuardPid();
                if(oldPid>1){
                    final int op=oldPid;
                    runOnUiThread(()->{
                        serviceRunning=true; updateStatus();
                        appendLog("[系统] Guard 服务已在运行，无需重复启动（pid="+op+"）");
                        showFloat("服务已在运行");
                    });
                    return;
                }
                File exe=ensureBinary();
                String q=shq(exe.getAbsolutePath());
                String cfg=shq(cfgFile.getAbsolutePath());
                // 启动时先删掉陈旧 pidfile，再用 nohup 后台挂起，避免读日志线程阻塞
                File pf=pidFile(); if(pf.exists()) pf.delete();
                String cmd="rm -f "+shq(pf.getAbsolutePath())+" ; chmod 755 "+q+" ; nohup "+q+" "+cfg+" > "+shq(new File(getFilesDir(),"guard.log").getAbsolutePath())+" 2>&1 &";
                int code=suExec(cmd).code;
                if(code!=0){
                    runOnUiThread(()->appendLog("[错误] 启动命令执行失败（code="+code+"）"));
                    return;
                }
                // 最多等 1.5 秒，轮询 pidfile + kill-0 双确认启动成功
                int waitPid=0;
                for(int i=0;i<15;i++){
                    try{ Thread.sleep(100); }catch(Exception e){}
                    int p=findGuardPid();
                    if(p>1){ waitPid=p; break; }
                }
                final int okPid=waitPid;
                if(okPid>1){
                    runOnUiThread(()->{
                        serviceRunning=true; updateStatus();
                        appendLog("[系统] Guard 服务启动成功（pid="+okPid+"，配置="+cfgFile.getAbsolutePath()+"）");
                        showFloat("服务已启动");
                    });
                }else{
                    // 启动后没等到 pid：把实时日志前 20 行贴出来便于排查崩溃点
                    StringBuilder lg=new StringBuilder();
                    try{
                        java.io.File logf=new File(getFilesDir(),"guard.log");
                        if(logf.exists()){
                            try(java.io.BufferedReader br=new java.io.BufferedReader(new java.io.FileReader(logf))){
                                String ln; int n=0;
                                while((ln=br.readLine())!=null && n++<24) lg.append(ln).append('\n');
                            }
                        }
                    }catch(Exception ignored){}
                    final String tail=lg.length()>0?lg.toString():"（无输出）";
                    runOnUiThread(()->{
                        serviceRunning=false; updateStatus();
                        appendLog("[错误] Guard 启动后未进入存活状态（pidfile 未出现/进程退出）。崩溃日志:\n"+tail);
                        showFloat("服务启动失败");
                    });
                }
            }catch(Exception e){
                runOnUiThread(()->appendLog("[错误] 启动异常："+e.getMessage()));
            }
        }).start();
    }

    void stopService(){
        appendLog("[系统] 正在停止服务…");
        new Thread(()->{
            int pid=0; String errMsg=null;
            try{
                pid=findGuardPid();
                boolean had=false;
                // 1) 按真实 PID 精准发信号（先礼后兵）
                if(pid>1){
                    had=true;
                    if(suExec("kill -TERM "+pid+" ; sleep 0.3 ; if kill -0 "+pid+" 2>/dev/null; then kill -KILL "+pid+" ; fi ; rm -f "+shq(pidFile().getAbsolutePath())).code!=0){
                        errMsg="（按 pid 发信号返回码异常）";
                    }
                    try{ Thread.sleep(200); }catch(Exception ignored){}
                }
                // 2) 兜底：pkill 清同名僵尸（不管 findGuardPid 有没有）
                had = had || (suExec("pkill -KILL -x "+GUARD_NAME+" ; pkill -0 -x "+GUARD_NAME+" ; echo $?").code==0);
                final boolean had2=had;
                // 3) 最终校验存活状态
                int stillPid=findGuardPid();
                final boolean stillRunning=stillPid>1;
                final int finalPid=stillPid;
                final String finalErr=errMsg;
                runOnUiThread(()->{
                    serviceRunning=stillRunning; updateStatus();
                    if(stillRunning){
                        appendLog("[警告] 服务可能未完全停止（剩余 pid="+finalPid+"）"+(finalErr!=null?finalErr:""));
                        showFloat("服务可能未完全停止");
                    }else{
                        File pf=pidFile(); if(pf.exists()) pf.delete();
                        appendLog("[系统] 服务已停止"+(had2?"（已发送信号）":"（本就未运行）"));
                        showFloat("服务已停止");
                    }
                });
            }catch(Exception e){
                String msg=e.getMessage();
                runOnUiThread(()->{ appendLog("[错误] 停止失败："+msg); showFloat("停止失败"); });
            }
        }).start();
    }

    // ==================== 配置 ====================
    // 勾选应用即自动写入配置（静默，避免频繁刷日志）
    void writeConfig(){
        // —— 互斥防线（写入前最终校验）：内存中同时处于 T+F 状态的应用，强制目标优先 ——
        HashSet<String> tSet=new HashSet<>(), fSet=new HashSet<>();
        for(AppInfo a: allApps){
            if(a.checkedT){ tSet.add(a.pkg); a.checkedF=false; }
        }
        // Guard 自身不得作为目标应用写入（兜底，理论上 UI 层已拦截）
        tSet.remove(getPackageName());
        for(AppInfo a: allApps){
            if(a.checkedF) fSet.add(a.pkg);
        }
        ArrayList<String> t=new ArrayList<>(tSet);
        ArrayList<String> f=new ArrayList<>(fSet);
        StringBuilder s=new StringBuilder(); s.append("# Guard config\ninterval:2\n\n");
        for(String p:t)s.append("target:").append(p).append('\n'); s.append('\n');
        for(String p:f)s.append("freeze:").append(p).append('\n'); s.append('\n');
        // 应用 Linux UID：C 守护在 GUARD_TASK 环境变量失效时（如脚本修改 environ），可作兜底识别条件
        s.append("appuid:").append(android.os.Process.myUid()).append('\n');
        final String content=s.toString();
        try{
            try{
                writeConfigFile(cfgFile, content);
            }catch(Exception e){
                // 配置文件可能由 root（Guard 默认配置）创建，普通写入被拒（EACCES）。
                // 应用拥有 files/ 目录：删除不要求文件本身可写，删除后重建即可
                if(cfgFile.exists() && !cfgFile.delete())
                    appendLog("[配置] 清理旧配置失败，改用 root 写入");
                writeConfigFile(cfgFile, content);
            }
            appendLog("[配置] 已保存：目标 "+t.size()+"，禁用 "+f.size()+" → "+cfgFile.getAbsolutePath());
        }catch(Exception e){
            if(rootGranted){
                try{
                    writeConfigAsRoot(content);
                    appendLog("[配置] 已保存(root)：目标 "+t.size()+"，禁用 "+f.size()+" → "+cfgFile.getAbsolutePath());
                }catch(Exception e2){
                    appendLog("[错误] 保存配置失败："+e.getMessage());
                }
            }else{
                appendLog("[错误] 保存配置失败："+e.getMessage());
            }
        }
    }

    void writeConfigFile(File f, String content) throws IOException{
        FileWriter w=new FileWriter(f);
        w.write(content); w.close();
    }

    // root 兜底写入：写完后 chmod 666，保证后续应用可直接覆写
    void writeConfigAsRoot(String content) throws Exception{
        Process p=new ProcessBuilder("su","-c","cat > "+shq(cfgFile.getAbsolutePath())+" && chmod 666 "+shq(cfgFile.getAbsolutePath())).start();
        OutputStream os=p.getOutputStream();
        os.write(content.getBytes("UTF-8"));
        os.flush(); os.close();
        if(p.waitFor()!=0) throw new IOException("root 写入失败");
    }

    // ==================== 日志框 ====================
    // 持久化写：将字符串追加到目标文件，超过 MAX(512KB) 截断为后 256KB（简单环形裁剪），保证写入稳定
    static final int MAX_LOG_SZ = 512 * 1024;
    static final int TRIM_TO   = 256 * 1024;
    void appendPersist(File f, String raw) {
        if (f == null || raw == null) return;
        try {
            long len = f.length();
            if (len + raw.length() > MAX_LOG_SZ) {
                // 截断：读后 TRIM_TO 字节，覆盖写
                byte[] all = new byte[0];
                if (f.exists()) {
                    java.io.FileInputStream fi = new java.io.FileInputStream(f);
                    java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                    long skip = len > TRIM_TO ? len - TRIM_TO : 0;
                    fi.skip(skip);
                    byte[] b = new byte[4096];
                    int n;
                    while ((n = fi.read(b)) > 0) bo.write(b, 0, n);
                    fi.close();
                    all = bo.toByteArray();
                }
                java.io.FileOutputStream fo = new java.io.FileOutputStream(f, false);
                if (all.length > 0) fo.write(all);
                fo.write(raw.getBytes("UTF-8"));
                fo.flush(); fo.getFD().sync(); fo.close();
            } else {
                java.io.FileOutputStream fo = new java.io.FileOutputStream(f, true);
                fo.write(raw.getBytes("UTF-8"));
                fo.flush(); fo.getFD().sync(); fo.close();
            }
        } catch (Throwable ignored) {}
    }

    // 用于：C 守护日志(guard.log)/历史载入，**不写盘**不重复持久化。
    // 规则：只有 UI 主动 appendLog（带 [HH:mm:ss.SSS] 前缀）会写 ui.log；
    //       守护日志本来就持久在 guard.log，若再 appendPersist(ui.log)，
    //       重开 loadHistoricLogs 时 guard.log→ui.log→再读 ui.log→指数级重复。
    void appendRawLog(String line){
        if(line==null||line.isEmpty()) return;
        runOnUiThread(()->{
            synchronized (logLock) {
                logBuffer.append(line).append("\n");
                if(logBuffer.length()>30000) logBuffer.delete(0,15000);
                logView.setText(logBuffer.toString());
                if(logAtBottom) logScroll.post(()->logScroll.fullScroll(View.FOCUS_DOWN));
            }
        });
    }

    void appendLog(final String line){
        // 在调用线程取时（更贴近事件发生时刻），并精确到毫秒
        final String ts=new SimpleDateFormat("HH:mm:ss.SSS", Locale.getDefault()).format(new Date());
        final String formatted="["+ts+"] "+line;
        runOnUiThread(()->{
            synchronized (logLock) {
                logBuffer.append(formatted).append("\n");
                if(logBuffer.length()>30000) logBuffer.delete(0,15000);
                logView.setText(logBuffer.toString());
                if(logAtBottom) logScroll.post(()->logScroll.fullScroll(View.FOCUS_DOWN));
            }
        });
        appendPersist(uiLogFile(), formatted+"\n");
    }

    void clearLog(){
        synchronized (logLock) {
            logBuffer.setLength(0);
            logView.setText("");
        }
        // 清持久化：创建空文件覆盖
        try{ new java.io.FileOutputStream(uiLogFile(),false).close(); }catch(Throwable ignored){}
        try{ new java.io.FileOutputStream(guardLogFile(),false).close(); guardLogOffset=0; }catch(Throwable ignored){}
    }

    // 载入历史日志：先守护(guard.log 后 256KB) 再 UI(ui.log 后 256KB)，避免 App 被杀就看不到过去的排查信息。
    // 注：都使用"字节级读 + UTF-8 边界检测"，避免 BufferedReader/InputStreamReader 内部
    // 按 8192 字节块切读时把多字节字符截断产生 U+FFFD �；也避免读 guard.log 再写 ui.log 导致的指数重复。
    void loadHistoricLogs(){
        new Thread(()->{
            // (A) 载入守护日志 → 只塞内存，不写盘（守护日志本身就持久化在 guard.log）
            try{
                File gf=guardLogFile();
                if(gf.exists()&&gf.length()>0){
                    long len=gf.length();
                    long from = (len>TRIM_TO) ? (len-TRIM_TO) : 0L;
                    long want = len - from;
                    java.io.FileInputStream fi=new java.io.FileInputStream(gf);
                    fi.skip(from);
                    byte[] buf = new byte[(int)Math.min(want, (long)TRIM_TO + 1)];
                    int total = 0;
                    while (total < buf.length) {
                        int n = fi.read(buf, total, buf.length - total);
                        if (n <= 0) break;
                        total += n;
                    }
                    fi.close();
                    if (total > 0) {
                        // 若 from>0 且正好卡在 UTF-8 多字节中间，开头 1-3 字节是上一段的"后半续字节"
                        // （10xxxxxx 或无首字节的残片），跳过到下一个字符边界，避免 decode 出 �。
                        int start = 0;
                        for (int k = 0; k < Math.min(3, total); k++) {
                            int v = buf[k] & 0xFF;
                            if ((v & 0x80) == 0) { start = k; break; }                 // ASCII 头
                            if ((v & 0xE0) == 0xC0 || (v & 0xF0) == 0xE0 || (v & 0xF8) == 0xF0) { start = k; break; } // 多字节首
                            // 10xxxxxx：还在残缺区，继续
                        }
                        int keepBack = 0;
                        int scanEnd = Math.max(start, total - 3);
                        for (int k = total - 1; k >= scanEnd; k--) {
                            int v = buf[k] & 0xFF;
                            int need;
                            if      ((v & 0x80) == 0) need = 1;
                            else if ((v & 0xE0) == 0xC0) need = 2;
                            else if ((v & 0xF0) == 0xE0) need = 3;
                            else if ((v & 0xF8) == 0xF0) need = 4;
                            else continue;
                            int have = total - k;
                            if (have < need) keepBack = have;
                            break;
                        }
                        int cutAt = total - keepBack;
                        int usable = (cutAt > start) ? (cutAt - start) : 0;
                        if (usable > 0) {
                            String s = new String(buf, start, usable, "UTF-8");
                            // 按 \n 切行，直接手动塞到 logBuffer（不写 ui.log，避免指数重复）
                            int st = 0, batch = 0;
                            while (true) {
                                int nl = s.indexOf('\n', st);
                                String piece;
                                if (nl < 0) { piece = s.substring(st); } else { piece = s.substring(st, nl); }
                                if (!piece.isEmpty()) {
                                    final String line = piece;
                                    final boolean doSleep = ((++batch) % 250 == 0);
                                    runOnUiThread(()->{
                                        synchronized (logLock) {
                                            logBuffer.append(line).append("\n");
                                            if(logBuffer.length()>30000) logBuffer.delete(0,15000);
                                            logView.setText(logBuffer.toString());
                                            if(logAtBottom) logScroll.post(()->logScroll.fullScroll(View.FOCUS_DOWN));
                                        }
                                    });
                                    if (doSleep) { try{ Thread.sleep(10); }catch(Throwable ignored){} }
                                }
                                if (nl < 0) break;
                                st = nl + 1;
                            }
                        }
                    }
                    guardLogOffset = len; // 追日志从最新位置开始，历史不重复
                }
            }catch(Throwable ignored){}
            // (B) 载入 UI 日志 → 只塞内存（ui.log 里本来就是持久化的 UI 前缀行）
            try{
                File uf=uiLogFile();
                if(uf.exists()&&uf.length()>0){
                    long len=uf.length();
                    long from = (len>TRIM_TO) ? (len-TRIM_TO) : 0L;
                    long want = len - from;
                    java.io.FileInputStream fi=new java.io.FileInputStream(uf);
                    fi.skip(from);
                    byte[] buf = new byte[(int)Math.min(want, (long)TRIM_TO + 1)];
                    int total = 0;
                    while (total < buf.length) {
                        int n = fi.read(buf, total, buf.length - total);
                        if (n <= 0) break;
                        total += n;
                    }
                    fi.close();
                    if (total > 0) {
                        int start = 0;
                        for (int k = 0; k < Math.min(3, total); k++) {
                            int v = buf[k] & 0xFF;
                            if ((v & 0x80) == 0) { start = k; break; }
                            if ((v & 0xE0) == 0xC0 || (v & 0xF0) == 0xE0 || (v & 0xF8) == 0xF0) { start = k; break; }
                        }
                        int keepBack = 0;
                        int scanEnd = Math.max(start, total - 3);
                        for (int k = total - 1; k >= scanEnd; k--) {
                            int v = buf[k] & 0xFF;
                            int need;
                            if      ((v & 0x80) == 0) need = 1;
                            else if ((v & 0xE0) == 0xC0) need = 2;
                            else if ((v & 0xF0) == 0xE0) need = 3;
                            else if ((v & 0xF8) == 0xF0) need = 4;
                            else continue;
                            int have = total - k;
                            if (have < need) keepBack = have;
                            break;
                        }
                        int cutAt = total - keepBack;
                        int usable = (cutAt > start) ? (cutAt - start) : 0;
                        if (usable > 0) {
                            String s = new String(buf, start, usable, "UTF-8");
                            int st = 0, batch = 0;
                            while (true) {
                                int nl = s.indexOf('\n', st);
                                String piece;
                                if (nl < 0) { piece = s.substring(st); } else { piece = s.substring(st, nl); }
                                if (!piece.isEmpty()) {
                                    final String line = piece;
                                    final boolean doSleep = ((++batch) % 250 == 0);
                                    runOnUiThread(()->{
                                        synchronized (logLock) {
                                            logBuffer.append(line).append("\n");
                                            if(logBuffer.length()>30000) logBuffer.delete(0,15000);
                                            logView.setText(logBuffer.toString());
                                            if(logAtBottom) logScroll.post(()->logScroll.fullScroll(View.FOCUS_DOWN));
                                        }
                                    });
                                    if (doSleep) { try{ Thread.sleep(10); }catch(Throwable ignored){} }
                                }
                                if (nl < 0) break;
                                st = nl + 1;
                            }
                        }
                    }
                }
            }catch(Throwable ignored){}
        }).start();
    }

    // 守护日志实时追：每 500ms 读 guard.log 的增量（比上次已知偏移多的部分），按行注入 UI。
    // 修复：原始做法固定大小 chunk+按\n切会导致 (1) 最后一行无\n时整段被退回→下次重复读同字节；
    // (2) 若 chunk 恰好在 UTF-8 多字节字符中间断开，new String(bytes,"UTF-8") 产生 U+FFFD �。
    // 解法：用 byte[] carry（上次残片）做前缀，先按 0x0A 切出整行；对最后一段不完整部分，
    // 做 UTF-8 结尾截断扫描：若末尾 1-3 字节是一个不完整的多字节序列前缀，就留下不 decode，
    // 下次与新字节合并再解，彻底避免 �。
    void startLogTailer(){
        if (logTailerRunning || (logTailerThread!=null && logTailerThread.isAlive())) return;
        logTailerRunning=true;
        logTailerThread=new Thread(()->{
            long knownLen = 0;
            byte[] carry = new byte[0]; // 上次残片（无换行的尾部字节，可能含截断的 UTF-8）
            java.io.ByteArrayOutputStream tailBuf = new java.io.ByteArrayOutputStream(2048);
            while (logTailerRunning) {
                try {
                    File gf = guardLogFile();
                    if (!gf.exists()) {
                        guardLogOffset = 0; knownLen = 0;
                        carry = new byte[0];
                        try{ Thread.sleep(500); }catch(Throwable t_ign){} continue;
                    }
                    long nowLen = gf.length();
                    // 文件被截断/轮替（比如 clearLog 或外部删了重建）：从 0 重新追
                    if (nowLen < knownLen || nowLen < guardLogOffset) {
                        guardLogOffset = 0; knownLen = nowLen;
                        carry = new byte[0];
                    }
                    if (nowLen <= guardLogOffset) {
                        knownLen = nowLen;
                        try{ Thread.sleep(500); }catch(Throwable t_ign){} continue;
                    }
                    long from = guardLogOffset;
                    long want = nowLen - from;
                    java.io.FileInputStream fi = new java.io.FileInputStream(gf);
                    fi.skip(from);
                    // 一次最多读 64KB，避免追落后把主线程灌爆
                    if (want > 65536) want = 65536;
                    byte[] b = new byte[4096];
                    tailBuf.reset();
                    long got = 0;
                    while (got < want) {
                        int toRead = (int)Math.min((long)b.length, want - got);
                        int n = fi.read(b, 0, toRead);
                        if (n <= 0) break;
                        tailBuf.write(b, 0, n); got += n;
                    }
                    fi.close();
                    int newBytesN = tailBuf.size();
                    if (newBytesN > 0 || carry.length > 0) {
                        // 合并 carry + 新读入字节
                        byte[] merged = new byte[carry.length + newBytesN];
                        if (carry.length > 0) System.arraycopy(carry, 0, merged, 0, carry.length);
                        if (newBytesN > 0) System.arraycopy(tailBuf.toByteArray(), 0, merged, carry.length, newBytesN);

                        int st = 0;
                        while (true) {
                            int nl = -1;
                            for (int k = st; k < merged.length; k++) {
                                if (merged[k] == (byte)'\n') { nl = k; break; }
                            }
                            if (nl < 0) break;
                            int lineLen = nl - st;
                            if (lineLen > 0) {
                                // 整行：不会卡在 UTF-8 边界中间（换行是 ASCII，必在字符边界）
                                String ln = new String(merged, st, lineLen, "UTF-8");
                                if (!ln.isEmpty()) appendRawLog(ln);
                            }
                            st = nl + 1;
                        }
                        // 尾部残片：没换行的最后一段，需检测 UTF-8 截断位置
                        int tailLen = merged.length - st;
                        if (tailLen == 0) {
                            carry = new byte[0];
                            guardLogOffset = from + got;
                        } else {
                            // 从末尾往回最多看 3 字节（UTF-8 单字符最长 4 字节），
                            // 找到"多字节首字节"位置与其应有的字符长度比对，
                            // 若该首字节之后剩余字节不足其声明长度，就是被截断的起点。
                            int keepBack = 0;
                            int scanStart = Math.max(st, merged.length - 3);
                            for (int k = merged.length - 1; k >= scanStart; k--) {
                                int v = merged[k] & 0xFF;
                                int need;
                                if      ((v & 0x80) == 0) { need = 1; } // 0xxxxxxx 单字节
                                else if ((v & 0xE0) == 0xC0) { need = 2; } // 110xxxxx  2字节
                                else if ((v & 0xF0) == 0xE0) { need = 3; } // 1110xxxx  3字节
                                else if ((v & 0xF8) == 0xF0) { need = 4; } // 11110xxx  4字节
                                else { continue; } // 10xxxxxx 续字节，继续往前找首字节
                                int have = merged.length - k;
                                if (have < need) {
                                    keepBack = have;
                                }
                                break;
                            }
                            int cutAt = merged.length - keepBack;
                            int emitLen = cutAt - st;
                            if (emitLen > 0) {
                                String lastEmit = new String(merged, st, emitLen, "UTF-8");
                                // 如果这段末尾无换行，理论上是 cutAt 截断产生的，但 cutAt 已经落在
                                // 字符边界上，仍可能包含若干整行（若 keepBack 之前还隐藏着换行）。
                                // 由于外层已把所有 \n 处理完，这里剩下的就是无换行的一串字符。
                                // 若 contain \n 说明 keepBack 前的位置不合理（不可能），再兜底切。
                                int lastNl = lastEmit.lastIndexOf('\n');
                                if (lastNl >= 0) {
                                    String pre = lastEmit.substring(0, lastNl);
                                    String post = lastEmit.substring(lastNl + 1);
                                    for (String piece : pre.split("\n")) {
                                        if (!piece.isEmpty()) appendRawLog(piece);
                                    }
                                    // post 退回到 carry
                                    byte[] postBytes = post.getBytes("UTF-8");
                                    byte[] newCarry = new byte[postBytes.length + keepBack];
                                    System.arraycopy(postBytes, 0, newCarry, 0, postBytes.length);
                                    if (keepBack > 0) System.arraycopy(merged, cutAt, newCarry, postBytes.length, keepBack);
                                    carry = newCarry;
                                } else {
                                    // 无换行且是字符边界：整段作为 carry（下次读到换行再 emit）
                                    byte[] newCarry = new byte[emitLen + keepBack];
                                    System.arraycopy(merged, st, newCarry, 0, emitLen);
                                    if (keepBack > 0) System.arraycopy(merged, cutAt, newCarry, emitLen, keepBack);
                                    carry = newCarry;
                                }
                            } else {
                                // emitLen==0：全部字节属于"待补足的 UTF-8 残片"，原样 carry
                                byte[] newCarry = new byte[keepBack];
                                if (keepBack > 0) System.arraycopy(merged, cutAt, newCarry, 0, keepBack);
                                carry = newCarry;
                            }
                            // 注意：只有真正处理完（已 decode+emit）的字节才推进 offset，
                            // carry 里的字节在当前文件流中已被读到，但在"逻辑消费层"还未 emit，
                            // 所以下次循环必须保留 carry，并推进 offset 到 from+got，
                            // 因为对应文件字节已经被读取并保存在 carry，不应再从文件重读。
                            guardLogOffset = from + got;
                        }
                    } else {
                        guardLogOffset = from + want;
                    }
                    knownLen = nowLen;
                    try{ Thread.sleep(500); }catch(Throwable t_ign){}
                }catch(Throwable outer_ign){
                    try{ Thread.sleep(500); }catch(Throwable t_ign){}
                }
            }
        },"GuardLogTailer");
        logTailerThread.setDaemon(true);
        logTailerThread.start();
    }

    @Override protected void onDestroy(){
        logTailerRunning=false;
        if(logTailerThread!=null) logTailerThread.interrupt();
        if(termProc!=null){ try{ termProc.destroy(); }catch(Exception ignored){} termProc=null; termIn=null; }
        super.onDestroy();
    }

    // 导出运行日志到储存空间根目录（/sdcard/）：
    // 优先 root 写入以绕过 Android 11+ 的分区存储限制，导出后在任意文件管理器可见
    // 合并 guard.log（守护全量）+ ui.log（App 操作全量），导出的 txt 是完整排查记录
    String concatAllLogs(){
        StringBuilder sb=new StringBuilder(256*1024);
        try{
            File gf=guardLogFile();
            if(gf.exists()){
                long len=gf.length(); long skip=(len>TRIM_TO)?(len-TRIM_TO):0;
                java.io.FileInputStream fi=new java.io.FileInputStream(gf);
                fi.skip(skip);
                sb.append("============ Guard Daemon Log (files/guard.log) ============\n");
                BufferedReader br=new BufferedReader(new InputStreamReader(fi,"UTF-8"),8192);
                String ln; while((ln=br.readLine())!=null){ sb.append(ln).append('\n'); }
                br.close(); fi.close();
                sb.append('\n');
            }
        }catch(Throwable ignored){}
        try{
            File uf=uiLogFile();
            if(uf.exists()){
                long len=uf.length(); long skip=(len>TRIM_TO)?(len-TRIM_TO):0;
                java.io.FileInputStream fi=new java.io.FileInputStream(uf);
                fi.skip(skip);
                sb.append("============ App UI Log (files/ui.log) ============\n");
                BufferedReader br=new BufferedReader(new InputStreamReader(fi,"UTF-8"),8192);
                String ln; while((ln=br.readLine())!=null){ sb.append(ln).append('\n'); }
                br.close(); fi.close();
            }
        }catch(Throwable ignored){}
        // 如果以上两份都为空，回退到内存 logBuffer（避免老版本升级后导出空内容）
        if(sb.length()==0){
            synchronized (logLock){ sb.append(logBuffer.toString()); }
        }
        return sb.toString();
    }
    void exportLog(){
        final String content=concatAllLogs();
        if(content.trim().isEmpty()){ showFloat("暂无日志可导出"); return; }
        final String name="Guard日志_"+new SimpleDateFormat("yyyyMMdd_HHmmss",Locale.getDefault()).format(new Date())+".txt";
        final String path="/sdcard/"+name;
        new Thread(()->{
            try{
                if(rootGranted){
                    // 先写应用缓存，再用 root cp 到 /sdcard（最可靠）
                    File tmp=new File(getCacheDir(),"export_log.txt");
                    FileWriter tw=new FileWriter(tmp); tw.write(content); tw.close();
                    Res r=suExec("cp "+shq(tmp.getAbsolutePath())+" "+shq(path)+" && chmod 666 "+shq(path));
                    tmp.delete();
                    if(r.code==0){ finishExport(path); return; }
                    // 回退：root cat 直接写目标
                    Process p=new ProcessBuilder("su","-c","cat > "+shq(path)).start();
                    OutputStream os=p.getOutputStream();
                    os.write(content.getBytes("UTF-8"));
                    os.flush(); os.close();
                    if(p.waitFor()==0){ finishExport(path); return; }
                    throw new IOException("root 写入失败："+r.out.trim());
                }
                // 无 root：尝试直接写入（Android 10 及以下一般可写）
                FileWriter w=new FileWriter(path);
                w.write(content); w.close();
                finishExport(path);
            }catch(Exception e){
                runOnUiThread(()->{
                    appendLog("[错误] 导出日志失败："+e.getMessage());
                    showFloat("导出失败");
                });
            }
        }).start();
    }

    void finishExport(final String path){
        runOnUiThread(()->{
            appendLog("[系统] 日志已导出："+path);
            showFloat("日志已导出到存储根目录");
        });
    }

    void openBugReport(){
        try{
            Intent i=new Intent(Intent.ACTION_VIEW,
                Uri.parse("https://qm.qq.com/q/yox95mY2PY"));
            i.setPackage("com.tencent.mobileqq"); // 优先用 QQ 打开
            if(i.resolveActivity(getPackageManager())==null)
                i.setPackage(null); // 未安装 QQ 时回退浏览器
            startActivity(i);
        }catch(Exception e){
            appendLog("[错误] 打开反馈链接失败："+e.getMessage());
        }
    }

    // ==================== 工作原理 ====================
    View principleHeader(String s){
        TextView v=tv(s,15,PRIMARY,true);
        v.setPadding(0,dp(14),0,dp(4));
        return v;
    }
    View principleItem(String s){
        TextView v=tv("· "+s,14,TEXT,false);
        v.setPadding(dp(8),dp(3),dp(6),dp(3));
        v.setLineSpacing(dp(2),1f);
        return v;
    }
    void showPrinciple(){
        ScrollView sc=new ScrollView(this);
        LinearLayout box=dialogBox(14);
        sc.addView(box);

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

        new AlertDialog.Builder(this)
            .setTitle("工作原理")
            .setView(sc)
            .setPositiveButton("已了解",null)
            .show();
    }

    // ==================== 脚本管理 & 轻量终端 ====================
    // 保存/恢复上次浏览的脚本目录路径（自动持久化）
    void loadScriptPath(){
        try{
            if(scriptPathPref.exists()){
                BufferedReader r=new BufferedReader(new FileReader(scriptPathPref));
                String s=r.readLine(); r.close();
                if(s!=null&&!s.trim().isEmpty()) scriptPath=s.trim();
            }
        }catch(Exception ignored){}
    }
    void saveScriptPath(){
        try{
            FileWriter w=new FileWriter(scriptPathPref);
            w.write(scriptPath); w.close();
        }catch(Exception ignored){}
    }

    void buildScripts(){
        // 结构完全对齐 picker（已验证可正常显示）：顶栏 + 滚动区；输入栏放在滚动区内并显式 MATCH_PARENT
        LinearLayout topbar=new LinearLayout(this);
        topbar.setOrientation(LinearLayout.HORIZONTAL);
        topbar.setGravity(Gravity.CENTER_VERTICAL);
        topbar.setPadding(dp(6),dp(6),dp(16),dp(6));
        topbar.setBackground(getDrawable(R.drawable.bg_topbar));

        ImageView back=new ImageView(this);
        back.setImageDrawable(icon(R.drawable.ic_back,TEXT));
        back.setPadding(dp(12),dp(8),dp(14),dp(8));
        topbar.addView(back);
        TextView title=tv("脚本管理",20,TEXT,true);
        topbar.addView(title);
        scriptContent.addView(topbar);
        back.setOnClickListener(v->showHome());

        scriptScroll=new ScrollView(this);
        LinearLayout body=new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(16),dp(14),dp(16),dp(32));
        scriptScroll.addView(body);
        scriptContent.addView(scriptScroll,new LinearLayout.LayoutParams(-1,0,1f));

        // 路径输入栏：显式 MATCH_PARENT 宽度，保证权重输入框正常占满
        LinearLayout pathRow=new LinearLayout(this);
        pathRow.setOrientation(LinearLayout.HORIZONTAL);
        pathRow.setGravity(Gravity.CENTER_VERTICAL);
        scriptPathInput=new EditText(this);
        scriptPathInput.setHint("输入脚本目录路径，如 /sdcard/scripts"); scriptPathInput.setTextSize(13);
        scriptPathInput.setHintTextColor(SUBTEXT); scriptPathInput.setTextColor(TEXT);
        scriptPathInput.setSingleLine(true); scriptPathInput.setTypeface(Typeface.MONOSPACE);
        scriptPathInput.setBackground(getDrawable(R.drawable.bg_search));
        scriptPathInput.setPadding(dp(14),dp(11),dp(14),dp(11));
        scriptPathInput.setImeOptions(EditorInfo.IME_ACTION_GO);
        scriptPathInput.setOnEditorActionListener((v,a,ev)->{
            if(a==EditorInfo.IME_ACTION_GO||a==EditorInfo.IME_ACTION_DONE){ goScriptPath(); return true; }
            return false;
        });
        LinearLayout.LayoutParams plp=new LinearLayout.LayoutParams(0,-2,1f);
        pathRow.addView(scriptPathInput,plp);
        Button go=new Button(this);
        go.setText("打开"); go.setTextColor(0xFFFFFFFF); go.setTextSize(13);
        go.setAllCaps(false); go.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        go.setBackground(round(0xFF2FA66F,dp(12)));
        LinearLayout.LayoutParams glp=new LinearLayout.LayoutParams(dp(80),dp(46));
        glp.setMargins(dp(10),0,0,0);
        pathRow.addView(go,glp);
        body.addView(pathRow,new LinearLayout.LayoutParams(-1,-2));
        go.setOnClickListener(v->goScriptPath());
        spring(go);

        TextView tip=tv("操作：输入脚本目录路径后点「打开」；点击文件夹逐级浏览；点击脚本文件即可运行（可勾选使用 SU 权限）。默认目录为上次保存的路径。",12,SUBTEXT,false);
        tip.setLineSpacing(dp(2),1f);
        tip.setPadding(dp(2),dp(12),dp(2),dp(6));
        body.addView(tip);

        scriptFileBox=new LinearLayout(this); scriptFileBox.setOrientation(LinearLayout.VERTICAL);
        body.addView(scriptFileBox);
    }

    void goScriptPath(){
        String p=scriptPathInput.getText().toString().trim();
        if(p.isEmpty()){ appendLog("[脚本] 请输入目录路径"); return; }
        scriptPath=p;
        saveScriptPath();
        refreshFileList();
    }

    // 以文件列表形式展开当前目录（文件夹在前，进入可逐级下钻）
    void refreshFileList(){
        scriptPathInput.setText(scriptPath);
        scriptFileBox.removeAllViews();
        File dir=new File(scriptPath);
        File parent=dir.getParentFile();
        if(parent!=null){
            LinearLayout up=new LinearLayout(this);
            up.setOrientation(LinearLayout.HORIZONTAL);
            up.setGravity(Gravity.CENTER_VERTICAL);
            up.setPadding(dp(2),dp(9),dp(2),dp(9));
            up.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE),null,null));
            ImageView uic=new ImageView(this);
            uic.setImageDrawable(icon(R.drawable.ic_up,TEXT));
            uic.setPadding(dp(4),dp(4),dp(8),dp(4));
            up.addView(uic);
            up.addView(tv("上一级  "+parent.getAbsolutePath(),14,TEXT,false));
            scriptFileBox.addView(up);
            up.setOnClickListener(v->{ scriptPath=parent.getAbsolutePath(); saveScriptPath(); refreshFileList(); });
            spring(up);
        }
        // 列目录：优先 Java File API；无权限读取时回退 root 读取（任意目录均可浏览）
        List<FsEntry> entries=new ArrayList<FsEntry>();
        boolean javaOk=false;
        try{
            File[] fs=dir.listFiles();
            if(fs!=null){
                javaOk=true;
                for(File f:fs){ FsEntry e=new FsEntry(f.getName(),f.isDirectory()); e.size=f.length(); entries.add(e); }
            }
        }catch(Exception ignored){}
        if(!javaOk&&rootGranted) entries=rootList(scriptPath);
        if(entries==null||entries.isEmpty()){
            String msg=entries==null?"无法读取该目录：无访问权限或路径无效"
                                    :(javaOk?"目录为空":"目录为空或无法读取（可尝试勾选 root 权限）");
            TextView e=tv(msg,14,SUBTEXT,false);
            e.setGravity(Gravity.CENTER); e.setPadding(0,dp(30),0,dp(30));
            scriptFileBox.addView(e);
            return;
        }
        Collections.sort(entries,(a,b)->{
            if(a.isDir!=b.isDir) return a.isDir?-1:1;
            return a.name.compareToIgnoreCase(b.name);
        });
        for(final FsEntry e:entries){
            LinearLayout row=new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(4),dp(8),dp(4),dp(8));
            row.setBackground(new RippleDrawable(ColorStateList.valueOf(RIPPLE),null,null));
            ImageView ic=new ImageView(this);
            ic.setImageDrawable(icon(e.isDir?R.drawable.ic_folder:R.drawable.ic_file,
                e.isDir?0xFF4A84E8:0xFF2FA66F));
            LinearLayout.LayoutParams ilp=new LinearLayout.LayoutParams(dp(34),dp(34));
            ilp.setMargins(0,0,dp(12),0);
            row.addView(ic,ilp);
            LinearLayout texts=new LinearLayout(this); texts.setOrientation(LinearLayout.VERTICAL);
            texts.addView(tv(e.name,14,TEXT,true));
            String sub;
            if(e.isDir) sub="文件夹";
            else sub=e.size>0?(e.size<1024?e.size+" B":String.format("%.1f KB",e.size/1024.0)):"文件";
            texts.addView(tv(sub,12,SUBTEXT,false));
            LinearLayout.LayoutParams tlp=new LinearLayout.LayoutParams(0,-2,1f);
            row.addView(texts,tlp);
            scriptFileBox.addView(row);
            final File target=new File(dir,e.name);
            row.setOnClickListener(v->{
                if(e.isDir){ scriptPath=target.getAbsolutePath(); saveScriptPath(); refreshFileList(); }
                else runScriptDialog(target);
            });
            spring(row);
        }
    }

    // 通过 root 读取目录内容（对 /data、/sdcard 等无权限目录也有效）
    List<FsEntry> rootList(String path){
        List<FsEntry> out=new ArrayList<FsEntry>();
        try{
            Res r=suExec("ls -Ap --color=never "+shq(path));
            if(r.code!=0) return null;
            if(r.out==null) return out;
            for(String line:r.out.split("\n")){
                String s=line.trim();
                if(s.isEmpty()||s.equals(".")||s.equals("..")) continue;
                boolean isDir=s.endsWith("/");
                String name=isDir?s.substring(0,s.length()-1):s;
                if(name.isEmpty()) continue;
                out.add(new FsEntry(name,isDir));
            }
            return out;
        }catch(Exception e){ return null; }
    }

    // 点开脚本：弹出运行确认，带「使用 SU 权限」复选框
    void runScriptDialog(final File f){
        LinearLayout box=dialogBox(18);
        TextView info=tv(f.getAbsolutePath(),13,SUBTEXT,false);
        info.setTypeface(Typeface.MONOSPACE);
        info.setPadding(0,0,0,dp(8));
        box.addView(info);
        TextView hint=tv("运行时会弹出实时窗口：程序出现「请输入/选择」等提示时，在底部输入框输入数字或内容后点「发送」即可。",12,SUBTEXT,false);
        hint.setLineSpacing(dp(2),1f); hint.setPadding(0,0,0,dp(12));
        box.addView(hint);
        final CheckBox su=new CheckBox(this);
        su.setText("使用 SU 权限运行（root）"); su.setChecked(true);
        su.setTextColor(TEXT);
        su.setButtonTintList(ColorStateList.valueOf(PRIMARY));
        box.addView(su);
        new AlertDialog.Builder(this)
            .setTitle("运行脚本 · "+f.getName())
            .setView(box)
            .setPositiveButton("运行",(d,w)->runScript(f,su.isChecked()))
            .setNegativeButton("取消",null)
            .show();
    }

    void runScript(final File f, final boolean useSu){
        final boolean binary=isElf(f);
        appendLog("[脚本] "+(useSu?"SU":"普通")+"运行："+f.getAbsolutePath()+(binary?"（检测为二进制可执行文件，直接执行）":""));

        // ===== 交互式运行窗口：顶部实时输出 + 底部输入框（可随时发送数字/内容给进程）=====
        LinearLayout box=dialogBox(14);

        ScrollView sc=new ScrollView(this);
        sc.setVerticalScrollBarEnabled(true);
        final TextView out=new TextView(this);
        out.setTextSize(13); out.setTypeface(Typeface.MONOSPACE); out.setTextColor(TEXT);
        out.setLineSpacing(dp(2),1f);
        sc.addView(out);
        LinearLayout.LayoutParams slp=new LinearLayout.LayoutParams(-1,dp(300));
        box.addView(sc,slp);

        LinearLayout inputRow=new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setGravity(Gravity.CENTER_VERTICAL);
        final EditText input=new EditText(this);
        input.setHint("输入数字或内容，点「发送」"); input.setTextSize(13);
        input.setHintTextColor(SUBTEXT); input.setTextColor(TEXT);
        input.setSingleLine(true); input.setTypeface(Typeface.MONOSPACE);
        input.setBackground(getDrawable(R.drawable.bg_search));
        input.setPadding(dp(12),dp(8),dp(12),dp(8));
        input.setImeOptions(EditorInfo.IME_ACTION_SEND);
        LinearLayout.LayoutParams ilp=new LinearLayout.LayoutParams(0,-2,1f);
        inputRow.addView(input,ilp);
        Button send=new Button(this);
        send.setText("发送"); send.setTextColor(0xFFFFFFFF); send.setTextSize(13);
        send.setAllCaps(false); send.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        send.setBackground(round(PRIMARY,dp(12)));
        LinearLayout.LayoutParams blp=new LinearLayout.LayoutParams(dp(72),dp(42));
        blp.setMargins(dp(8),dp(12),0,0);
        inputRow.addView(send,blp);
        box.addView(inputRow);

        final Process[] proc={null};
        final boolean[] finished={false};
        final long[] gotPid={-1L};
        final long[] suPid={-1L};
        final boolean[] userClosed={false};

        final Runnable sendInput=()->{
            if(proc[0]==null||finished[0]){ liveAppend(out,sc,"\n[进程已结束，无法发送]"); return; }
            String s=input.getText().toString();
            input.setText("");
            if(s.trim().isEmpty()) return;
            try{
                proc[0].getOutputStream().write((s+"\n").getBytes("UTF-8"));
                proc[0].getOutputStream().flush();
                liveAppend(out,sc,"> "+s);
            }catch(Exception e){ liveAppend(out,sc,"\n[发送失败："+e.getMessage()+"]"); }
        };
        send.setOnClickListener(v->sendInput.run());
        input.setOnEditorActionListener((v,a,ev)->{ if(a==EditorInfo.IME_ACTION_SEND){ sendInput.run(); return true; } return false; });

        final AlertDialog dlg=new AlertDialog.Builder(this)
            .setTitle("运行 · "+f.getName()+"（可随时关闭窗口取消）")
            .setView(box)
            .setPositiveButton("关闭窗口",null)
            .create();
        dlg.setOnDismissListener(d->{ userClosed[0]=true; if(proc[0]!=null){ try{ proc[0].destroy(); }catch(Exception ignored){} } });
        dlg.show();
        input.requestFocus();
        input.postDelayed(()->showKeyboard(input),200);

        new Thread(()->{
            Process p=null;
            BufferedReader r=null;
            try{
                String dir=(f.getParentFile()!=null)?f.getParentFile().getAbsolutePath():"/";
                String path=shq(f.getAbsolutePath());
                String dirq=shq(dir);
                if(useSu){
                    // App 进程无权读取 /data 等目录，必须在 root shell 内判定 ELF 并执行：
                    // 读前 4 字节十六进制，等于 7f454c46（\x7fELF）即为二进制，直接 chmod+exec；否则按 sh 脚本运行。
                    // 首行输出 __GUARD_PID__=$$（shell 自身 PID），随后 exec 替换进程，
                    // 保证该 PID 即真正执行脚本的 SH/二进制进程，用于清理日志。
                    // GUARD_TASK=1 提前 export：无论走 ELF 分支还是 sh 脚本分支，后代进程 environ 都会带上，便于守护识别并清理。
                    String run=
                        "export GUARD_TASK=1; echo __GUARD_PID__=$$; "+
                        "magic=$(head -c4 "+path+" 2>/dev/null | od -An -tx1 | tr -d ' \\n'); "+
                        "if [ \"$magic\" = \"7f454c46\" ]; then "+
                            "cd "+dirq+" && chmod +x "+path+" && export LD_LIBRARY_PATH="+dirq+":\\\"$LD_LIBRARY_PATH\\\" && exec "+path+"; "+
                        "else "+
                            "cd "+dirq+" && "+SCRIPT_ENV+" && exec /system/bin/sh "+path+"; "+
                        "fi";
                    p=new ProcessBuilder("su","-c",run).redirectErrorStream(true).start();
                }else{
                    ProcessBuilder pb;
                    if(binary){
                        f.setExecutable(true,false);
                        pb=new ProcessBuilder(f.getAbsolutePath());
                    }else{
                        pb=new ProcessBuilder("/system/bin/sh",f.getAbsolutePath());
                    }
                    pb.directory(f.getParentFile());
                    Map<String,String> e=pb.environment();
                    e.put("PATH",DEF_PATH); e.put("HOME","/data/local/tmp");
                    e.put("LANG","en_US.UTF-8"); e.put("TERM","xterm");
                    e.put("GUARD_TASK","1");
                    p=pb.redirectErrorStream(true).start();
                }
                proc[0]=p;
                suPid[0]=pidOf(p);
                r=new BufferedReader(new InputStreamReader(p.getInputStream(),"UTF-8"));
                String line;
                while((line=r.readLine())!=null){
                    // 解析 shell 上报的自身 PID（首行标记），不显示
                    if(line.startsWith("__GUARD_PID__=")){
                        try{ gotPid[0]=Long.parseLong(line.substring("__GUARD_PID__=".length()).trim()); }
                        catch(Exception ignored){}
                        continue;
                    }
                    final String s=line;
                    runOnUiThread(()->liveAppend(out,sc,s));
                }
                int code=p.waitFor();
                final int rc=code;
                runOnUiThread(()->{
                    finished[0]=true;
                    liveAppend(out,sc,"\n[执行完毕，退出码 "+rc+"]");
                    dlg.setTitle("运行结束 · "+f.getName());
                    appendLog("[脚本] 执行完毕，退出码 "+rc);
                });
            }catch(Exception e){
                final String msg=e.getMessage();
                final boolean cancelled=userClosed[0];
                runOnUiThread(()->{
                    finished[0]=true;
                    liveAppend(out,sc,"\n["+(cancelled?"已取消（进程被关闭）":("执行失败："+msg))+"]");
                    if(!cancelled) appendLog("[脚本] 执行失败："+msg);
                });
            }finally{
                // 进程结束后清理管道与进程，释放文件描述符，避免残留 shell / 僵尸进程
                boolean hadProcess = p != null;
                if(r!=null){ try{ r.close(); }catch(Exception ignored){} }
                if(p!=null){
                    try{ p.getOutputStream().close(); }catch(Exception ignored){}
                    try{ p.getInputStream().close(); }catch(Exception ignored){}
                    try{ p.destroy(); }catch(Exception ignored){}
                }
                proc[0]=null;
                if(hadProcess){
                    // 清理本次运行产生的所有遗留进程（包括但不限于 su、sh 及其子进程），并附 PID 记录日志
                    long sh = gotPid[0]>0 ? gotPid[0] : (useSu ? -1L : suPid[0]);
                    cleanupLeftovers(suPid[0], sh, f.getName());
                }
            }
        }).start();
    }

    // 实时输出区追加一行并自动滚到底部（自动过滤 ANSI 颜色/控制转义码）
    void liveAppend(final TextView out, final ScrollView sc, final String s){
        String t=cleanAnsi(s);
        if(t.isEmpty()) return;
        out.append(t+"\n");
        sc.post(()->sc.fullScroll(View.FOCUS_DOWN));
    }

    // 过滤 ANSI 转义序列（如 \x1b[1;32m、\x1b[0m、\x1b[36m）及尾部回车，避免原始转义码显示为乱码
    String cleanAnsi(String s){
        if(s==null) return "";
        String t=s.replaceAll("\u001B\\[[0-9;?]*[ -/]*[@-~]","")
                 .replaceAll("\u001B\\][^\\u0007\\u001B]*(\\u0007|\\u001B\\\\)","");
        int end=t.length(); while(end>0&&t.charAt(end-1)=='\r') end--;
        return t.substring(0,end);
    }

    // 检测 ELF 文件头：某些“.sh”实为编译好的二进制可执行文件，不应交给 sh 解释（否则出现 ELF 乱码报错）
    boolean isElf(File f){
        try{
            FileInputStream in=new FileInputStream(f);
            byte[] b=new byte[4];
            int n=in.read(b);
            in.close();
            return n==4&&b[0]==0x7F&&b[1]=='E'&&b[2]=='L'&&b[3]=='F';
        }catch(Exception e){ return false; }
    }

    // ===== 轻量终端（交互式 root shell）=====
    void buildTerminal(){
        LinearLayout topbar=new LinearLayout(this);
        topbar.setOrientation(LinearLayout.HORIZONTAL);
        topbar.setGravity(Gravity.CENTER_VERTICAL);
        topbar.setPadding(dp(6),dp(6),dp(16),dp(6));
        topbar.setBackground(getDrawable(R.drawable.bg_topbar));

        ImageView back=new ImageView(this);
        back.setImageDrawable(icon(R.drawable.ic_back,TEXT));
        back.setPadding(dp(12),dp(8),dp(14),dp(8));
        topbar.addView(back);
        TextView title=tv("脚本终端",20,TEXT,true);
        topbar.addView(title);
        LinearLayout.LayoutParams tsp=new LinearLayout.LayoutParams(0,-2,1f);
        topbar.addView(new View(this),tsp);
        Button clear=mkSmallBtn("清屏",SUBTEXT);
        topbar.addView(clear);
        terminalContent.addView(topbar);
        back.setOnClickListener(v->showHome());
        clear.setOnClickListener(v->termOut.setText(""));

        termScroll=new ScrollView(this);
        termScroll.setVerticalScrollBarEnabled(true);
        termOut=new TextView(this);
        termOut.setTextSize(13);
        termOut.setTextColor(0xFFD8F0D8);
        termOut.setTypeface(Typeface.MONOSPACE);
        termOut.setLineSpacing(dp(2),1f);
        termOut.setPadding(dp(12),dp(12),dp(12),dp(12));
        termOut.setBackground(round(0xFF10141C,dp(16)));
        termScroll.addView(termOut);
        LinearLayout.LayoutParams tl=new LinearLayout.LayoutParams(-1,0,1f);
        tl.setMargins(dp(14),dp(12),dp(14),dp(4));
        terminalContent.addView(termScroll,tl);

        LinearLayout inputRow=new LinearLayout(this);
        inputRow.setOrientation(LinearLayout.HORIZONTAL);
        inputRow.setGravity(Gravity.CENTER_VERTICAL);
        inputRow.setPadding(dp(14),dp(6),dp(14),dp(14));
        termInput=new EditText(this);
        termInput.setHint("输入命令，回车执行"); termInput.setTextSize(14);
        termInput.setHintTextColor(0xFF7C8694); termInput.setTextColor(0xFF2C3138);
        termInput.setSingleLine(true); termInput.setTypeface(Typeface.MONOSPACE);
        termInput.setBackground(getDrawable(R.drawable.bg_search));
        termInput.setPadding(dp(16),dp(10),dp(16),dp(10));
        termInput.setImeOptions(EditorInfo.IME_ACTION_SEND);
        termInput.setOnEditorActionListener((v,a,ev)->{ if(a==EditorInfo.IME_ACTION_SEND){ termSend(); return true; } return false; });
        LinearLayout.LayoutParams ilp=new LinearLayout.LayoutParams(0,-2,1f);
        inputRow.addView(termInput,ilp);
        termSendBtn=new Button(this);
        termSendBtn.setText("发送"); termSendBtn.setTextColor(0xFFFFFFFF);
        termSendBtn.setTextSize(14); termSendBtn.setAllCaps(false);
        termSendBtn.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        termSendBtn.setBackground(round(0xFF2FA66F,dp(14)));
        LinearLayout.LayoutParams blp=new LinearLayout.LayoutParams(dp(76),dp(44));
        blp.setMargins(dp(10),0,0,0);
        inputRow.addView(termSendBtn,blp);
        terminalContent.addView(inputRow);
        termSendBtn.setOnClickListener(v->termSend());
    }

    void showKeyboard(View v){
        try{
            InputMethodManager im=(InputMethodManager)getSystemService(INPUT_METHOD_SERVICE);
            im.showSoftInput(v,InputMethodManager.SHOW_IMPLICIT);
        }catch(Exception ignored){}
    }

    // 启动交互式 shell（有 root 走 su 进入 root shell，否则回退普通 sh），使用系统环境
    void termStart(){
        if(termProc!=null) return;
        appendTerm("$ 正在启动终端…\n");
        new Thread(()->{
            try{
                ProcessBuilder pb;
                if(rootGranted) pb=new ProcessBuilder("su","-c",SCRIPT_ENV+" && sh");
                else{
                    pb=new ProcessBuilder("/system/bin/sh");
                    Map<String,String> e=pb.environment();
                    e.put("PATH",DEF_PATH); e.put("HOME","/data/local/tmp");
                    e.put("LANG","en_US.UTF-8"); e.put("TERM","xterm");
                    e.put("GUARD_TASK","1");
                }
                pb.redirectErrorStream(true);
                final Process p=pb.start();
                termProc=p;
                termIn=new BufferedWriter(new OutputStreamWriter(p.getOutputStream()));
                appendTerm(rootGranted?"$ root@android:/ # 就绪（root shell），可直接输入命令\n"
                                       :"$ app@android:/ $ 就绪（普通 shell）\n");
                final String run=pendingRunScript; pendingRunScript=null;
                if(run!=null) termInput.postDelayed(()->termWrite(termScriptCmd(run)), 400);
                BufferedReader r=new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line;
                while((line=r.readLine())!=null){
                    final String s=line;
                    runOnUiThread(()->appendTerm(s+"\n"));
                }
                termProc=null; termIn=null;
                runOnUiThread(()->appendTerm("$ 终端已退出\n"));
            }catch(Exception e){
                runOnUiThread(()->appendTerm("[错误] 启动终端失败："+e.getMessage()+"\n"));
                termProc=null; termIn=null;
            }
        }).start();
    }

    void appendTerm(final String s){
        runOnUiThread(()->{
            termOut.append(cleanAnsi(s));
            termScroll.post(()->termScroll.fullScroll(View.FOCUS_DOWN));
        });
    }

    void termWrite(String cmd){
        if(termProc==null||termIn==null){ appendTerm("[错误] 终端未运行\n"); return; }
        appendTerm("$ "+cmd+"\n");
        try{ termIn.write(cmd+"\n"); termIn.flush(); }
        catch(Exception e){ appendTerm("[错误] 写入失败："+e.getMessage()+"\n"); }
    }

    void termSend(){
        String cmd=termInput.getText().toString();
        termInput.setText("");
        if(cmd.trim().isEmpty()) return;
        termWrite(cmd);
    }

    // ==================== 工具 ====================
    void ensureBinaryAsync(){
        new Thread(()->{ try{ ensureBinary(); }catch(Exception ignored){} }).start();
    }

    File ensureBinary() throws IOException {
        File f=new File(getFilesDir(),EXE_NAME);
        // 始终从 assets 覆盖写入，保证 APK 更新后新二进制立即生效（否则旧版会一直留在设备上）
        InputStream in=getAssets().open(ASSET);
        OutputStream os=new FileOutputStream(f);
        byte[] buf=new byte[16384]; int n;
        while((n=in.read(buf))>0) os.write(buf,0,n);
        os.close(); in.close();
        f.setExecutable(true,false);
        return f;
    }

    String shq(String s){ return "'"+s.replace("'","'\\''")+"'"; }

    // 获取进程 PID：优先标准 API，失败时反射读取内部 pid 字段（部分 ROM 不支持 p.pid()）
    static long pidOf(Process p){
        if(p==null) return -1L;
        try{ return p.pid(); }catch(Throwable ignored){}
        try{
            java.lang.reflect.Field f=p.getClass().getDeclaredField("pid");
            f.setAccessible(true);
            try{ return f.getLong(p); }catch(IllegalArgumentException e){ return f.getInt(p); }
        }catch(Throwable ignored){}
        return -1L;
    }

    // 清理本次脚本运行产生的所有遗留进程（包括但不限于 su、sh 及其子进程），并附 PID 记录日志。
    // 执行链已知进程（suPid=su 进程、shPid=脚本/二进制进程，exec 后即真实执行者）无论是否已退出，
    // 都按角色名写入日志；仍存活的遗留子进程按「cgroup 同属应用」扫描出来，记录真实进程名后结束。
    void cleanupLeftovers(final long suPid, final long shPid, final String scriptName){
        try{
            if(!rootGranted){
                appendLog("[脚本] 已清理所有遗留进程（无 root，跳过）");
                return;
            }
            // 已知执行链进程（去重：普通模式二者为同一 PID）
            final StringBuilder known=new StringBuilder();
            final StringBuilder report=new StringBuilder();
            if(suPid>0 && suPid!=shPid){
                known.append(suPid).append(' ');
                report.append(" PID=").append(suPid).append("（su）");
            }
            if(shPid>0){
                known.append(shPid).append(' ');
                String lbl=(scriptName!=null&&!scriptName.isEmpty())?scriptName:"sh";
                report.append(" PID=").append(shPid).append("（").append(lbl).append("）");
            }
            final String k=known.toString().trim();
            final String appPid=String.valueOf(android.os.Process.myPid());
            // ① 结束已知执行链进程（su / 脚本进程）；k 为空时整段跳过，避免 for 空列表
            final String knownKill=k.isEmpty()?"":
                "for kpid in "+k+"; do "+
                "  [ \"$kpid\" = \""+appPid+"\" ] && continue; "+
                "  [ \"$kpid\" = \"$$\" ] && continue; "+
                "  [ \"$kpid\" = \"$PPID\" ] && continue; "+
                "  kill -TERM \"$kpid\" 2>/dev/null; "+
                "done; ";
            final String script=
                // 取 /proc/self/cgroup 中最深路径作为应用 cgroup 标识（兼容 v1/v2 多行）
                "CG=$(awk -F: 'NF>1{print length($NF), $NF}' /proc/self/cgroup | sort -rn | head -1 | cut -d' ' -f2-); "+
                "UIDSEG=$(printf '%s' \"$CG\" | grep -o '/uid_[0-9]*' | head -1); "+
                "[ -z \"$CG\" ] && CG=__NO_CG__; "+
                knownKill+
                // ② 扫描仍存活的遗留子进程：cgroup 同属应用（路径相同/子级，或共享 uid 段），输出 PID 与进程名
                "for p in /proc/[0-9]*; do "+
                "  pid=${p#/proc/}; "+
                "  [ \"$pid\" = \""+appPid+"\" ] && continue; "+
                "  [ \"$pid\" = \"$$\" ] && continue; "+
                "  [ \"$pid\" = \"$PPID\" ] && continue; "+
                "  case \" "+k+" \" in *\" $pid \"*) continue;; esac; "+
                "  cg=$(cat \"$p/cgroup\" 2>/dev/null) || continue; "+
                "  hit=0; "+
                "  case \"$cg\" in \"$CG\"|\"$CG\"/*) hit=1;; esac; "+
                "  [ \"$hit\" = 0 ] && [ -n \"$UIDSEG\" ] && case \"$cg\" in *\"$UIDSEG\"*) hit=1;; esac; "+
                "  [ \"$hit\" = 1 ] || continue; "+
                "  name=$(cat \"$p/comm\" 2>/dev/null); "+
                "  echo \"$pid $name\"; "+
                "  kill -TERM \"$pid\" 2>/dev/null; "+
                "done; "+
                "sleep 1; "+
                // ③ 强杀仍未退出的
                "for p in /proc/[0-9]*; do "+
                "  pid=${p#/proc/}; "+
                "  [ \"$pid\" = \""+appPid+"\" ] && continue; "+
                "  [ \"$pid\" = \"$$\" ] && continue; "+
                "  [ \"$pid\" = \"$PPID\" ] && continue; "+
                "  case \" "+k+" \" in *\" $pid \"*) continue;; esac; "+
                "  cg=$(cat \"$p/cgroup\" 2>/dev/null) || continue; "+
                "  hit=0; "+
                "  case \"$cg\" in \"$CG\"|\"$CG\"/*) hit=1;; esac; "+
                "  [ \"$hit\" = 0 ] && [ -n \"$UIDSEG\" ] && case \"$cg\" in *\"$UIDSEG\"*) hit=1;; esac; "+
                "  [ \"$hit\" = 1 ] || continue; "+
                "  kill -KILL \"$pid\" 2>/dev/null; "+
                "done";
            Res r=suExec(script);
            if(r.out!=null){
                for(String ln:r.out.split("\n")){
                    String t=ln.trim();
                    if(t.isEmpty()) continue;
                    String[] parts=t.split("\\s+",2);
                    report.append(" PID=").append(parts[0]);
                    if(parts.length>1&&!parts[1].isEmpty())
                        report.append("（").append(parts[1].trim()).append("）");
                }
            }
            if(report.length()==0) appendLog("[脚本] 已清理所有遗留进程（无残留）");
            else appendLog("[脚本] 已清理所有遗留进程"+report);
        }catch(Exception e){
            appendLog("[脚本] 清理遗留进程失败："+e.getMessage());
        }
    }

    static class Res{ int code; String out; Res(int code,String out){ this.code=code; this.out=out; } }

    Res suExec(String cmd) throws Exception {
        Process p=new ProcessBuilder("su","-c",cmd).redirectErrorStream(true).start();
        String out=read(p.getInputStream());
        int code=p.waitFor();
        return new Res(code,out);
    }

    void runRoot(String command,String okMsg){
        new Thread(()->{
            try{
                Res r=suExec(command);
                appendLog(r.code==0?("[系统] "+okMsg):("操作失败："+r.out.trim()));
            }catch(Exception e){ appendLog("Root 执行失败："+e.getMessage()); }
        }).start();
    }

    static String read(InputStream in)throws Exception{
        BufferedReader r=new BufferedReader(new InputStreamReader(in));
        StringBuilder s=new StringBuilder(); String x;
        while((x=r.readLine())!=null)s.append(x).append('\n');
        return s.toString();
    }
}