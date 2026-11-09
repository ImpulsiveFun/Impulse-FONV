package org.fnvprobe;

import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.TextView;

final class LauncherStyle {
    static final int BACKGROUND = 0xff15130f;
    private static final int PANEL = 0xff211d16;
    private static final int BORDER = 0xff493d2b;
    private static final int TEXT = 0xfff8ebd4;
    private static final int MUTED = 0xffc6b9a3;
    private static final int AMBER = 0xffffc375;
    private static final Typeface DISPLAY = Typeface.create("sans-serif-condensed", Typeface.BOLD);

    private final Context context;

    LauncherStyle(Context context) {
        this.context = context;
    }

    int dp(int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    private GradientDrawable shape(int fill, int stroke, int radius) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setCornerRadius(dp(radius));
        if (stroke != Color.TRANSPARENT) drawable.setStroke(dp(1), stroke);
        return drawable;
    }

    LinearLayout page() {
        LinearLayout page = new LinearLayout(context) {
            @Override protected void onMeasure(int width, int height) {
                int available = MeasureSpec.getSize(width);
                int cap = dp(620);
                if (available > cap) width = MeasureSpec.makeMeasureSpec(cap, MeasureSpec.EXACTLY);
                else if (MeasureSpec.getMode(width) == MeasureSpec.AT_MOST)
                    width = MeasureSpec.makeMeasureSpec(available, MeasureSpec.EXACTLY);
                super.onMeasure(width, height);
            }
        };
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(16), dp(14), dp(16), dp(22));
        return page;
    }

    void addBlock(LinearLayout page, View view) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        if (page.getChildCount() > 0) params.topMargin = dp(12);
        page.addView(view, params);
    }

    View hero() {
        FrameLayout hero = new FrameLayout(context);
        hero.setBackground(shape(0xff34251b, BORDER, 16));
        hero.setClipToOutline(true);
        hero.setMinimumHeight(dp(152));

        ImageView scene = new ImageView(context);
        scene.setImageResource(R.drawable.launcher_mojave);
        scene.setScaleType(ImageView.ScaleType.CENTER_CROP);
        scene.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        hero.addView(scene, new FrameLayout.LayoutParams(dp(350), dp(152), Gravity.END | Gravity.BOTTOM));
        View shade = new View(context);
        shade.setBackground(new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[]{0xff211910, 0xdd211910, 0x05211910}));
        shade.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        hero.addView(shade, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout copy = new LinearLayout(context);
        copy.setOrientation(LinearLayout.VERTICAL);
        copy.setPadding(dp(19), dp(17), dp(18), dp(20));
        LinearLayout kicker = new LinearLayout(context);
        kicker.setGravity(Gravity.CENTER_VERTICAL);
        ImageView mark = new ImageView(context);
        mark.setImageResource(R.drawable.launcher_mark);
        mark.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        kicker.addView(mark, new LinearLayout.LayoutParams(dp(34), dp(34)));
        TextView platform = label("ANDROID EDITION", 11, AMBER);
        platform.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        platform.setLetterSpacing(.14f);
        kicker.addView(platform);
        copy.addView(kicker);
        TextView title = label("NEW VEGAS", 39, TEXT);
        title.setTypeface(DISPLAY);
        title.setLetterSpacing(.025f);
        if (Build.VERSION.SDK_INT >= 28) title.setAccessibilityHeading(true);
        copy.addView(title);
        TextView edition = label("RECOMP", 16, AMBER);
        edition.setTypeface(DISPLAY);
        edition.setLetterSpacing(.28f);
        copy.addView(edition);
        hero.addView(copy, new FrameLayout.LayoutParams(-1, -2, Gravity.CENTER_VERTICAL));
        return hero;
    }

    LinearLayout card(String heading) {
        LinearLayout card = new LinearLayout(context);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(shape(PANEL, BORDER, 14));
        card.setPadding(dp(14), dp(11), dp(14), dp(12));
        if (heading != null) {
            TextView title = label(heading, 11, AMBER);
            title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            title.setLetterSpacing(.14f);
            if (Build.VERSION.SDK_INT >= 28) title.setAccessibilityHeading(true);
            title.setPadding(dp(3), 0, dp(3), dp(5));
            card.addView(title);
        }
        return card;
    }

    TextView status() {
        TextView status = label("", 13, MUTED);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setPadding(dp(3), dp(8), dp(3), dp(2));
        return status;
    }

    private TextView label(String text, int size, int color) {
        TextView label = new TextView(context);
        label.setText(text);
        label.setTextSize(size);
        label.setTextColor(color);
        return label;
    }

    void styleRadio(RadioButton radio) {
        radio.setTextColor(TEXT);
        radio.setTextSize(15);
        radio.setMinimumHeight(dp(48));
        radio.setPadding(dp(3), 0, dp(4), 0);
    }

    Button button(String title, boolean primary) {
        Button button = new Button(context);
        button.setText(title);
        button.setAllCaps(false);
        button.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        button.setTextSize(primary ? 18 : 14);
        button.setMinimumHeight(dp(primary ? 52 : 48));
        button.setGravity(primary ? Gravity.CENTER : Gravity.CENTER_VERTICAL | Gravity.START);
        button.setPadding(dp(14), dp(7), dp(14), dp(7));
        button.setStateListAnimator(null);
        button.setTextColor(new ColorStateList(
                new int[][]{new int[]{-android.R.attr.state_enabled}, new int[0]},
                new int[]{0xff958774, primary ? 0xff26180e : TEXT}));

        StateListDrawable background = new StateListDrawable();
        background.addState(new int[]{-android.R.attr.state_enabled}, shape(0xff2a241b, BORDER, 9));
        background.addState(new int[]{android.R.attr.state_focused}, shape(primary ? 0xffffd7a2 : 0xff4b3726, AMBER, 9));
        background.addState(new int[0], shape(primary ? AMBER : 0xff30271d, primary ? Color.TRANSPARENT : BORDER, 9));
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(primary ? 0x3826180e : 0x40ffc375),
                background, shape(Color.WHITE, Color.TRANSPARENT, 9)));
        return button;
    }

    void addAction(LinearLayout card, View action) {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        if (card.getChildCount() > 0) params.topMargin = dp(6);
        card.addView(action, params);
    }
}
