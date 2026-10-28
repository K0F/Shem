/* =============================================================================
 * LiveCodeEditor.java - Ergonomic Monospace SuperCollider Live-Code Editor
 * Author: Jáchym Pešek
 * =============================================================================
 */

package org.kof.shem;

import android.content.Context;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.widget.EditText;

public class LiveCodeEditor extends EditText {

    public interface OnLiveCodeActionListener {
        void onEvaluate(String code);
        void onStopAll();
    }

    private OnLiveCodeActionListener actionListener;

    public LiveCodeEditor(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    private void init() {
        try {
            Typeface tf = Typeface.createFromAsset(getContext().getAssets(), "Monaco.ttf");
            setTypeface(tf);
        } catch (Exception e) {
            setTypeface(Typeface.MONOSPACE);
        }
        setTextSize(13.5f);
        setTextColor(0xFFE6E6E6); // Shem text moonlight
        setHighlightColor(0x80468CD2); // Shem active selection
        setBackgroundColor(0xFF161618);
        setIncludeFontPadding(false);
        setPadding(20, 20, 20, 20);
        setLineSpacing(6f, 1.15f);
    }

    public void setOnLiveCodeActionListener(OnLiveCodeActionListener l) {
        this.actionListener = l;
    }

    public String getActiveCodeBlock() {
        int start = getSelectionStart();
        int end = getSelectionEnd();
        CharSequence text = getText();
        if (text == null || text.length() == 0) return "";

        if (start != end && start >= 0 && end <= text.length()) {
            return text.subSequence(Math.min(start, end), Math.max(start, end)).toString().trim();
        }

        // Current line if no selection
        int lineStart = Math.max(0, start);
        while (lineStart > 0 && text.charAt(lineStart - 1) != '\n') {
            lineStart--;
        }
        int lineEnd = Math.max(0, start);
        while (lineEnd < text.length() && text.charAt(lineEnd) != '\n') {
            lineEnd++;
        }
        return text.subSequence(lineStart, lineEnd).toString().trim();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (actionListener != null) {
            boolean isCtrl = event.isCtrlPressed();
            boolean isShift = event.isShiftPressed();

            // Ctrl+Enter or Shift+Enter -> Evaluate!
            if (keyCode == KeyEvent.KEYCODE_ENTER && (isCtrl || isShift)) {
                String code = getActiveCodeBlock();
                if (!code.isEmpty()) {
                    actionListener.onEvaluate(code);
                }
                return true;
            }

            // Ctrl+. -> Panic Stop
            if (keyCode == KeyEvent.KEYCODE_PERIOD && isCtrl) {
                actionListener.onStopAll();
                return true;
            }
        }

        return super.onKeyDown(keyCode, event);
    }
}
