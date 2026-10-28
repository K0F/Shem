/* =============================================================================
 * MainActivity.java - Shem Live-Coding Environment for Android
 * Author: Jáchym Pešek
 * =============================================================================
 */

package org.kof.shem;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

public class MainActivity extends Activity {

    static {
        System.loadLibrary("shem");
    }

    private native boolean nativeInit(String username);
    private native String nativeEvaluate(String code);
    private native void nativeSetLang(boolean running);
    private native void nativeSetServer(boolean running);
    private native void nativeSetBpm(float bpm);
    private native void nativeGetStatus(float[] outStatus);
    private native int nativeGetNodeCount();
    private native String nativeGetNodeName(int index);
    private native void nativeGetNodeInfo(int index, float[] outInfo);
    private native void nativeSetNodeState(int index, int state);
    private native void nativeSetNodeVolume(int index, float vol);
    private native int nativeGetEnvPoints(int nodeIdx, float[] outTimes, float[] outLevels);
    private native void nativeSetEnvPoint(int nodeIdx, int ptIdx, float time, float level);
    private native void nativeSetEnvPreset(int nodeIdx, int presetType);
    private native String nativePopUdpMessage();
    private native void nativeClose();

    private LiveCodeEditor codeEditor;
    private ControlEnvelopeView envelopeView;
    private TextView tvTitle, tvAuthor, tvStatusInfo, tvConsole, tvNodesHeader, tvConsoleLabel;
    private Button btnLang, btnServer;
    private LinearLayout toolbarSymbols, toolbarEnvelope, panelNodes;

    private Typeface tfMonaco;
    private Typeface tfUnivers;

    private boolean langRunning = true;
    private boolean serverRunning = true;
    private int currentEnvNodeIndex = 0;

    private final float[] statusBuffer = new float[7];
    private final float[] nodeInfoBuffer = new float[6];
    private final float[] envTimesBuffer = new float[16];
    private final float[] envLevelsBuffer = new float[16];
    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private boolean isDestroyed = false;

    // Authentic Shem UI Palette
    private static final int COLOR_BG = 0xFF141416;
    private static final int COLOR_HEADER = 0xFF202024;
    private static final int COLOR_PANEL = 0xFF18181B;
    private static final int COLOR_CARD = 0xFF1F1F24;
    private static final int COLOR_BORDER = 0xFF35353A;
    private static final int COLOR_TEXT_PRIMARY = 0xFFE6E6E6;
    private static final int COLOR_TEXT_MUTED = 0xFF8899AA;
    private static final int COLOR_ACTIVE = 0xFF468CD2;     // Shem Blue
    private static final int COLOR_RESULT = 0xFFAAC8A0;     // Shem Mint
    private static final int COLOR_EVAL = 0xFF73DACA;       // Tokyo Cyan-Green
    private static final int COLOR_STOP = 0xFFF7768E;       // Shem / Tokyo Coral Red
    private static final int COLOR_WARN = 0xFFE6821E;       // Shem Amber

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        loadTypefaces();

        codeEditor = findViewById(R.id.code_editor);
        envelopeView = findViewById(R.id.envelope_view);
        tvTitle = findViewById(R.id.tv_title);
        tvAuthor = findViewById(R.id.tv_author);
        tvStatusInfo = findViewById(R.id.tv_status_info);
        tvConsole = findViewById(R.id.tv_console);
        tvNodesHeader = findViewById(R.id.tv_nodes_header);
        tvConsoleLabel = findViewById(R.id.tv_console_label);
        btnLang = findViewById(R.id.btn_lang);
        btnServer = findViewById(R.id.btn_server);
        toolbarSymbols = findViewById(R.id.toolbar_symbols);
        toolbarEnvelope = findViewById(R.id.toolbar_envelope);
        panelNodes = findViewById(R.id.panel_nodes);

        applyGlobalTypography();

        nativeInit("Jachym");

        initEditorContent();
        initSymbolToolbar();
        initControlEnvelope();
        initHeaderButtons();
        initListeners();

        startTelemetryLoop();
    }

    private void loadTypefaces() {
        try {
            tfMonaco = Typeface.createFromAsset(getAssets(), "Monaco.ttf");
        } catch (Exception e) {
            tfMonaco = Typeface.MONOSPACE;
        }

        try {
            tfUnivers = Typeface.createFromAsset(getAssets(), "Univers_57_condensed.ttf");
        } catch (Exception e) {
            tfUnivers = Typeface.SANS_SERIF;
        }
    }

    private void applyGlobalTypography() {
        if (tvTitle != null) tvTitle.setTypeface(tfUnivers, Typeface.BOLD);
        if (tvAuthor != null) tvAuthor.setTypeface(tfUnivers);
        if (tvStatusInfo != null) tvStatusInfo.setTypeface(tfUnivers);
        if (tvNodesHeader != null) tvNodesHeader.setTypeface(tfUnivers, Typeface.BOLD);
        if (tvConsoleLabel != null) tvConsoleLabel.setTypeface(tfUnivers, Typeface.BOLD);
        if (tvConsole != null) tvConsole.setTypeface(tfMonaco);
        if (btnLang != null) btnLang.setTypeface(tfUnivers, Typeface.BOLD);
        if (btnServer != null) btnServer.setTypeface(tfUnivers, Typeface.BOLD);
    }

    private int dpToPx(float dp) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, dp, getResources().getDisplayMetrics()));
    }

    private Button createShemButton(String text, int textColor, int bgColor, int borderColor, float textSizeSp, boolean isCodeFont) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(textColor);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, textSizeSp);
        b.setTypeface(isCodeFont ? tfMonaco : tfUnivers, isCodeFont ? Typeface.NORMAL : Typeface.BOLD);
        b.setAllCaps(false);
        b.setIncludeFontPadding(false);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        b.setStateListAnimator(null);
        b.setGravity(Gravity.CENTER);

        int padH = dpToPx(7);
        int padV = dpToPx(3);
        b.setPadding(padH, padV, padH, padV);

        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(bgColor);
        if (borderColor != 0) {
            gd.setStroke(dpToPx(1), borderColor);
        }
        gd.setCornerRadius(dpToPx(2));
        b.setBackground(gd);
        return b;
    }

    private void initHeaderButtons() {
        updateLangButton();
        updateServerButton();

        btnLang.setOnClickListener(v -> {
            langRunning = !langRunning;
            nativeSetLang(langRunning);
            updateLangButton();
        });

        btnServer.setOnClickListener(v -> {
            serverRunning = !serverRunning;
            nativeSetServer(serverRunning);
            updateServerButton();
        });
    }

    private void updateLangButton() {
        btnLang.setText(langRunning ? "LANG: ON" : "LANG: OFF");
        btnLang.setTextColor(langRunning ? 0xFFFFFFFF : COLOR_TEXT_MUTED);
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(langRunning ? COLOR_ACTIVE : 0x00000000);
        gd.setStroke(dpToPx(1), langRunning ? COLOR_ACTIVE : COLOR_BORDER);
        gd.setCornerRadius(dpToPx(2));
        btnLang.setBackground(gd);
    }

    private void updateServerButton() {
        btnServer.setText(serverRunning ? "SERVER: ON" : "SERVER: OFF");
        btnServer.setTextColor(serverRunning ? 0xFFFFFFFF : COLOR_TEXT_MUTED);
        GradientDrawable gd = new GradientDrawable();
        gd.setShape(GradientDrawable.RECTANGLE);
        gd.setColor(serverRunning ? COLOR_ACTIVE : 0x00000000);
        gd.setStroke(dpToPx(1), serverRunning ? COLOR_ACTIVE : COLOR_BORDER);
        gd.setCornerRadius(dpToPx(2));
        btnServer.setBackground(gd);
    }

    private void initEditorContent() {
        String template =
                "// Shem Live Coding Session\n" +
                "// Author: Jachym Pesek\n" +
                "p.bpm(124);\n\n" +
                "~lead = { Saw.ar(220) * 0.35 };\n" +
                "~wash = { SinOsc.ar([110, 112]) * 0.25 };\n" +
                "~bass = { MoogFF.ar(Pulse.ar(55), 450, 2.2) * 0.4 };\n";
        codeEditor.setText(template);
        codeEditor.setSelection(template.length());
    }

    private void initSymbolToolbar() {
        toolbarSymbols.removeAllViews();
        String[] symbols = {
                "EVAL", "STOP", "~", "{", "}", "[", "]", "(", ")",
                ";", ".", "\\", "|", "+", "*", "_", "=", "s.boot", "s.quit"
        };

        for (String sym : symbols) {
            Button b;
            if (sym.equals("EVAL")) {
                b = createShemButton(sym, 0xFFFFFFFF, COLOR_RESULT, 0, 11f, false);
                b.setOnClickListener(v -> evaluateCurrent());
            } else if (sym.equals("STOP")) {
                b = createShemButton(sym, 0xFFFFFFFF, COLOR_STOP, 0, 11f, false);
                b.setOnClickListener(v -> stopAll());
            } else {
                b = createShemButton(sym, COLOR_TEXT_PRIMARY, 0x18FFFFFF, COLOR_BORDER, 11f, true);
                b.setOnClickListener(v -> insertTextAtCursor(sym));
            }

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, dpToPx(26));
            lp.setMargins(dpToPx(2), dpToPx(3), dpToPx(2), dpToPx(3));
            b.setLayoutParams(lp);

            toolbarSymbols.addView(b);
        }
    }

    private void insertTextAtCursor(String text) {
        int start = Math.max(0, codeEditor.getSelectionStart());
        int end = Math.max(0, codeEditor.getSelectionEnd());
        codeEditor.getText().replace(Math.min(start, end), Math.max(start, end), text, 0, text.length());
    }

    private void initControlEnvelope() {
        envelopeView.setOnEnvelopeChangeListener((pointIndex, time, level) -> {
            nativeSetEnvPoint(currentEnvNodeIndex, pointIndex, time, level);
        });
        selectEnvelopeNode(0);
    }

    private void selectEnvelopeNode(int nodeIdx) {
        int count = nativeGetNodeCount();
        if (count == 0) return;
        currentEnvNodeIndex = Math.max(0, Math.min(count - 1, nodeIdx));

        String nodeName = nativeGetNodeName(currentEnvNodeIndex);
        envelopeView.setTitle(String.format("ControlEnvelope: %s (cutoff)", nodeName));

        int ptCount = nativeGetEnvPoints(currentEnvNodeIndex, envTimesBuffer, envLevelsBuffer);
        if (ptCount > 0) {
            envelopeView.setPointsFromArrays(envTimesBuffer, envLevelsBuffer, ptCount);
        }

        refreshEnvelopeToolbar();
    }

    private void refreshEnvelopeToolbar() {
        toolbarEnvelope.removeAllViews();
        int count = nativeGetNodeCount();

        // 1. Node Selector Chips (~lead, ~wash, ~bass)
        for (int i = 0; i < count; i++) {
            final int idx = i;
            String name = nativeGetNodeName(i);
            boolean isSel = (i == currentEnvNodeIndex);

            int bg = isSel ? COLOR_ACTIVE : 0x00000000;
            int textCol = isSel ? 0xFFFFFFFF : COLOR_TEXT_MUTED;
            int borderCol = isSel ? COLOR_ACTIVE : COLOR_BORDER;

            Button b = createShemButton(name, textCol, bg, borderCol, 10f, false);
            b.setOnClickListener(v -> selectEnvelopeNode(idx));

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, dpToPx(24));
            lp.setMargins(dpToPx(2), dpToPx(4), dpToPx(2), dpToPx(4));
            b.setLayoutParams(lp);
            toolbarEnvelope.addView(b);
        }

        // Divider
        View div = new View(this);
        div.setBackgroundColor(COLOR_BORDER);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(dpToPx(1), dpToPx(16));
        dlp.setMargins(dpToPx(6), dpToPx(8), dpToPx(6), dpToPx(8));
        div.setLayoutParams(dlp);
        toolbarEnvelope.addView(div);

        // 2. Preset Selector Chips
        final String[] presetNames = { "ADSR", "Sweep", "Tri", "Gate", "Wobble" };
        for (int p = 0; p < presetNames.length; p++) {
            final int presetId = p;
            Button b = createShemButton(presetNames[p], COLOR_TEXT_PRIMARY, 0x15FFFFFF, COLOR_BORDER, 9.5f, false);
            b.setOnClickListener(v -> {
                nativeSetEnvPreset(currentEnvNodeIndex, presetId);
                int ptCount = nativeGetEnvPoints(currentEnvNodeIndex, envTimesBuffer, envLevelsBuffer);
                if (ptCount > 0) {
                    envelopeView.setPointsFromArrays(envTimesBuffer, envLevelsBuffer, ptCount);
                }
            });

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, dpToPx(24));
            lp.setMargins(dpToPx(2), dpToPx(4), dpToPx(2), dpToPx(4));
            b.setLayoutParams(lp);
            toolbarEnvelope.addView(b);
        }
    }

    private void initListeners() {
        codeEditor.setOnLiveCodeActionListener(new LiveCodeEditor.OnLiveCodeActionListener() {
            @Override
            public void onEvaluate(String code) {
                evaluateCode(code);
            }

            @Override
            public void onStopAll() {
                stopAll();
            }
        });
    }

    private void evaluateCurrent() {
        String code = codeEditor.getActiveCodeBlock();
        if (!code.isEmpty()) {
            evaluateCode(code);
        }
    }

    private void evaluateCode(String code) {
        String result = nativeEvaluate(code);
        tvConsole.setText(result);
        refreshNodePanel();
        refreshEnvelopeToolbar();
    }

    private void stopAll() {
        String result = nativeEvaluate("s.quit");
        tvConsole.setText(result);
        refreshNodePanel();
        refreshEnvelopeToolbar();
    }

    private void refreshNodePanel() {
        panelNodes.removeAllViews();
        int count = nativeGetNodeCount();

        for (int i = 0; i < count; i++) {
            final int nodeIdx = i;
            String name = nativeGetNodeName(i);
            nativeGetNodeInfo(i, nodeInfoBuffer);
            int state = (int) nodeInfoBuffer[0];
            float vol = nodeInfoBuffer[1];

            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.HORIZONTAL);
            card.setGravity(Gravity.CENTER_VERTICAL);
            card.setPadding(dpToPx(8), dpToPx(5), dpToPx(8), dpToPx(5));

            GradientDrawable cardBg = new GradientDrawable();
            cardBg.setShape(GradientDrawable.RECTANGLE);
            cardBg.setColor((nodeIdx == currentEnvNodeIndex) ? 0xFF24242A : COLOR_CARD);
            cardBg.setStroke(dpToPx(1), (nodeIdx == currentEnvNodeIndex) ? COLOR_ACTIVE : COLOR_BORDER);
            cardBg.setCornerRadius(dpToPx(3));
            card.setBackground(cardBg);

            LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            clp.setMargins(0, 0, 0, dpToPx(6));
            card.setLayoutParams(clp);

            // Node Name (clicking selects envelope target)
            TextView tv = new TextView(this);
            tv.setText(name);
            tv.setTextColor(state == 1 ? COLOR_ACTIVE : COLOR_TEXT_MUTED);
            tv.setTypeface(tfUnivers, Typeface.BOLD);
            tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            tv.setIncludeFontPadding(false);
            LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(
                    dpToPx(72), ViewGroup.LayoutParams.WRAP_CONTENT);
            tv.setLayoutParams(tlp);
            card.addView(tv);

            // Play / Stop Button
            boolean isPlaying = (state == 1);
            int btnColor = isPlaying ? COLOR_ACTIVE : 0x00000000;
            int btnTextCol = isPlaying ? 0xFFFFFFFF : COLOR_TEXT_MUTED;
            int borderCol = isPlaying ? COLOR_ACTIVE : COLOR_BORDER;
            Button btnToggle = createShemButton(isPlaying ? "PLAY" : "STOP", btnTextCol, btnColor, borderCol, 10f, false);

            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(dpToPx(52), dpToPx(24));
            blp.setMargins(0, 0, dpToPx(8), 0);
            btnToggle.setLayoutParams(blp);
            btnToggle.setOnClickListener(v -> {
                int nextState = isPlaying ? 2 : 1;
                nativeSetNodeState(nodeIdx, nextState);
                refreshNodePanel();
            });
            card.addView(btnToggle);

            // Volume Slider
            SeekBar bar = new SeekBar(this);
            bar.setMax(100);
            bar.setProgress((int)(vol * 100));
            bar.setLayoutParams(new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                    if (fromUser) nativeSetNodeVolume(nodeIdx, progress / 100.0f);
                }
                @Override public void onStartTrackingTouch(SeekBar seekBar) {}
                @Override public void onStopTrackingTouch(SeekBar seekBar) {}
            });
            card.addView(bar);

            // Clicking card selects node for envelope editing
            card.setOnClickListener(v -> {
                selectEnvelopeNode(nodeIdx);
                refreshNodePanel();
            });

            panelNodes.addView(card);
        }
    }

    private void startTelemetryLoop() {
        uiHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (isDestroyed) return;

                nativeGetStatus(statusBuffer);
                int bpm = (int) statusBuffer[2];
                int beat = (int) statusBuffer[3];
                float cpu = statusBuffer[4];
                int synths = (int) statusBuffer[5];
                float peak = statusBuffer[6];

                tvStatusInfo.setText(String.format("%d BPM | Beat: %d | Synths: %d | %.0f dB | CPU: %.1f%%",
                        bpm, beat, synths, peak, cpu));

                // Poll UDP Peer Broadcasts
                String peerMsg = nativePopUdpMessage();
                if (peerMsg != null) {
                    tvConsole.setText(peerMsg);
                }

                uiHandler.postDelayed(this, 100); // 10 Hz UI refresh
            }
        }, 100);

        refreshNodePanel();
    }

    @Override
    protected void onDestroy() {
        isDestroyed = true;
        nativeClose();
        super.onDestroy();
    }
}
