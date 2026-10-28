/* =============================================================================
 * ControlEnvelopeView.java - Interactive Breakpoint Envelope Editor
 * Author: Jáchym Pešek
 * =============================================================================
 */

package org.kof.shem;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

public class ControlEnvelopeView extends View {

    public interface OnEnvelopeChangeListener {
        void onPointChanged(int pointIndex, float time, float level);
    }

    private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pointPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint gridPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    public static class Point {
        public float time;  // 0.0 to 1.0
        public float level; // 0.0 to 1.0
        public Point(float t, float l) { this.time = t; this.level = l; }
    }

    private final List<Point> points = new ArrayList<>();
    private int selectedPoint = -1;
    private OnEnvelopeChangeListener listener;
    private String title = "ControlEnvelope (~lead:cutoff)";

    public ControlEnvelopeView(Context context, AttributeSet attrs) {
        super(context, attrs);

        gridPaint.setColor(0x28787878);
        gridPaint.setStrokeWidth(1.5f);

        linePaint.setColor(0xFF468CD2); // Authentic Shem Active Blue
        linePaint.setStrokeWidth(3.0f);
        linePaint.setStyle(Paint.Style.STROKE);

        fillPaint.setColor(0x28468CD2);
        fillPaint.setStyle(Paint.Style.FILL);

        pointPaint.setColor(0xFFFFFFFF);
        pointPaint.setStyle(Paint.Style.FILL);

        try {
            android.graphics.Typeface tf = android.graphics.Typeface.createFromAsset(context.getAssets(), "Univers_57_condensed.ttf");
            textPaint.setTypeface(tf);
        } catch (Exception ignored) {}

        textPaint.setColor(0xFFE6E6E6);
        textPaint.setTextSize(26f);
        textPaint.setFakeBoldText(true);

        // Default envelope points
        points.add(new Point(0.0f, 0.2f));
        points.add(new Point(0.25f, 1.0f));
        points.add(new Point(0.75f, 0.4f));
        points.add(new Point(1.0f, 0.2f));
    }

    public void setOnEnvelopeChangeListener(OnEnvelopeChangeListener l) {
        this.listener = l;
    }

    public void setTitle(String t) {
        this.title = t;
        postInvalidate();
    }

    public void setPoints(List<Point> newPoints) {
        points.clear();
        points.addAll(newPoints);
        postInvalidate();
    }

    public void setPointsFromArrays(float[] times, float[] levels, int count) {
        points.clear();
        for (int i = 0; i < count; i++) {
            points.add(new Point(times[i], levels[i]));
        }
        postInvalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) return;

        float pad = 24f;
        float plotW = w - 2 * pad;
        float plotH = h - 2 * pad;

        // Draw Grid
        for (int i = 0; i <= 4; i++) {
            float y = pad + plotH * (i / 4.0f);
            canvas.drawLine(pad, y, pad + plotW, y, gridPaint);
            float x = pad + plotW * (i / 4.0f);
            canvas.drawLine(x, pad, x, pad + plotH, gridPaint);
        }

        // Draw Envelope Path
        if (points.size() >= 2) {
            Path path = new Path();
            Path fill = new Path();

            float startX = pad + points.get(0).time * plotW;
            float startY = pad + (1.0f - points.get(0).level) * plotH;
            path.moveTo(startX, startY);
            fill.moveTo(startX, pad + plotH);
            fill.lineTo(startX, startY);

            for (int i = 1; i < points.size(); i++) {
                float px = pad + points.get(i).time * plotW;
                float py = pad + (1.0f - points.get(i).level) * plotH;
                path.lineTo(px, py);
                fill.lineTo(px, py);
            }

            fill.lineTo(pad + points.get(points.size() - 1).time * plotW, pad + plotH);
            fill.close();

            canvas.drawPath(fill, fillPaint);
            canvas.drawPath(path, linePaint);

            // Draw Breakpoints
            for (int i = 0; i < points.size(); i++) {
                float px = pad + points.get(i).time * plotW;
                float py = pad + (1.0f - points.get(i).level) * plotH;
                pointPaint.setColor((i == selectedPoint) ? 0xFFFFFFFF : 0xFFFFCC33);
                canvas.drawCircle(px, py, (i == selectedPoint) ? 14f : 9f, pointPaint);
            }
        }

        canvas.drawText(title, pad + 8f, pad + 24f, textPaint);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int w = getWidth();
        int h = getHeight();
        float pad = 24f;
        float plotW = w - 2 * pad;
        float plotH = h - 2 * pad;

        float x = event.getX();
        float y = event.getY();

        switch (event.getAction()) {
            case MotionEvent.ACTION_DOWN:
                selectedPoint = -1;
                float minDist = 48f * 48f; // touch radius
                for (int i = 0; i < points.size(); i++) {
                    float px = pad + points.get(i).time * plotW;
                    float py = pad + (1.0f - points.get(i).level) * plotH;
                    float dist = (px - x) * (px - x) + (py - y) * (py - y);
                    if (dist < minDist) {
                        minDist = dist;
                        selectedPoint = i;
                    }
                }
                invalidate();
                return true;

            case MotionEvent.ACTION_MOVE:
                if (selectedPoint >= 0 && selectedPoint < points.size()) {
                    float t = (x - pad) / plotW;
                    float l = 1.0f - (y - pad) / plotH;

                    if (selectedPoint == 0) t = 0.0f;
                    else if (selectedPoint == points.size() - 1) t = 1.0f;
                    else {
                        t = Math.max(points.get(selectedPoint - 1).time + 0.02f,
                                Math.min(points.get(selectedPoint + 1).time - 0.02f, t));
                    }

                    l = Math.max(0.0f, Math.min(1.0f, l));

                    points.get(selectedPoint).time = t;
                    points.get(selectedPoint).level = l;

                    if (listener != null) {
                        listener.onPointChanged(selectedPoint, t, l);
                    }
                    invalidate();
                }
                return true;

            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                selectedPoint = -1;
                invalidate();
                return true;
        }

        return super.onTouchEvent(event);
    }
}
