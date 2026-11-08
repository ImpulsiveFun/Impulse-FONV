package org.fnvprobe;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.drawable.GradientDrawable;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;
import java.util.ArrayList;

/** Sends touch, keyboard, mouse and Android gamepad events to the game's input bridge. */
final class InputOverlay extends FrameLayout implements InputManager.InputDeviceListener {
    final View movementPad, lookArea;
    final Button fireButton, aimButton;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final InputManager inputManager;
    private final SharedPreferences controlPreferences;
    private final int[] keySources = new int[256];
    private final int[] mouseSources = new int[8];
    private boolean active;
    private boolean showTouch = true;
    private final Button padMode, look;
    private final ArrayList<HudControl> hud = new ArrayList<>();
    private HudControl selectedControl;
    private SeekBar resizeControl;
    private TextView selectedLabel;
    private boolean editing;
    private int editPointer = -1;
    private float editX, editY;
    private float lastX, lastY, distance;
    private boolean menuTap;
    private int lookPointer = -1;
    private float hoverX = Float.NaN, hoverY = Float.NaN;
    private static final int NO_GAMEPAD = Integer.MIN_VALUE;
    private int gamepadId = NO_GAMEPAD;
    private boolean gamepadEnabled = true;
    private int lookSensitivity;
    private int touchSensitivity;
    private float aimRemainderX, aimRemainderY;
    private int gamepadButtons, gamepadHat;
    private int leftX, leftY, rightX, rightY, leftTrigger, rightTrigger;
    private boolean leftTriggerButton, rightTriggerButton;
    private final Runnable endTap = () -> mouse(0, false, TAP);
    private final Runnable endCamera = () -> key(CHANGE_VIEW, false, CAMERA);
    private static final int TOUCH = 1, KEYBOARD = 2, TAP = 4, MOUSE = 8;
    private static final int CAMERA = 16, CHANGE_VIEW = 0x21;
    private static final int FORWARD = 0x11, LEFT = 0x1e, BACK = 0x1f, RIGHT = 0x20;

    InputOverlay(Context context, SurfaceView screen) {
        super(context);
        inputManager = context.getSystemService(InputManager.class);
        controlPreferences = context.getSharedPreferences("game-controls", Context.MODE_PRIVATE);
        lookSensitivity = Math.max(50, Math.min(200, controlPreferences.getInt("right-stick-sensitivity", 100)));
        touchSensitivity = Math.max(25, Math.min(300, controlPreferences.getInt("touch-aim-sensitivity", 100)));
        setMotionEventSplittingEnabled(true);
        lookArea = new View(context);
        lookArea.setOnTouchListener((view, event) -> touch(event));
        lookArea.setOnGenericMotionListener((view, event) -> mouseMotion(event));
        addView(lookArea, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        movementPad = new MovementPad(context);
        addControl("stick", "Movement stick", movementPad, 168, 142, 16, 18, false, true);
        fireButton = mouseButton("Fire", 0, 76);
        aimButton = mouseButton("Aim", 1, 62);
        addControl("fire", "Fire", fireButton, 76, 60, 15, 75, true, true);
        addControl("aim", "Aim", aimButton, 62, 48, 97, 81, true, true);
        addControl("reload", "Reload", keyButton("Reload", 0x13, 68), 68, 48, 15, 141, true, true);
        addControl("vats", "VATS", keyButton("VATS", 0x2f, 62), 62, 48, 97, 141, true, true);
        addControl("jump", "Jump", keyButton("Jump", 0x39, 68), 68, 48, 15, 21, true, true);
        addControl("use", "Use", keyButton("Use", 0x12, 62), 62, 48, 97, 21, true, true);
        addControl("pause", "Pause", keyButton("Pause", 1, 66), 66, 48, 15, 13, false, false);
        addControl("pipboy", "Pip-Boy", keyButton("Pip-Boy", 0x0f, 76), 76, 48, 87, 13, false, false);
        addControl("crouch", "Crouch", keyButton("Crouch", 0x1d, 72), 72, 48, 169, 13, false, false);
        addControl("run", "Run", keyButton("Run", 0x2a, 54), 54, 48, 247, 13, false, false);
        addControl("ok", "OK", keyButton("OK", 0x1c, 46), 46, 48, 307, 13, false, false);
        Button camera = button("View", 62);
        camera.setContentDescription("Switch first-person or third-person view");
        camera.setOnClickListener(view -> {
            if (!active) return;
            handler.removeCallbacks(endCamera);
            key(CHANGE_VIEW, true, CAMERA);
            handler.postDelayed(endCamera, 100);
        });
        addControl("view", "View", camera, 62, 48, 359, 13, false, false);
        LinearLayout menus = new LinearLayout(context);
        padMode = button("Pad On", 66);
        padMode.setOnClickListener(view -> {
            gamepadEnabled = !gamepadEnabled;
            padMode.setText(gamepadEnabled ? "Pad On" : "Pad Off");
            if (gamepadEnabled) refreshGamepad();
            else releaseGamepad();
        });
        menus.addView(padMode);
        look = button("Look", 56);
        look.setOnClickListener(view -> showLookSettings(false));
        menus.addView(look);
        LayoutParams menuPosition = new LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.RIGHT);
        menuPosition.setMargins(dp(12), dp(10), 0, 0);
        addView(menus, menuPosition);
        if (screen != null) {
            screen.setFocusableInTouchMode(true);
            screen.setOnGenericMotionListener((view, event) -> mouseMotion(event));
        }
        setOnApplyWindowInsetsListener((view, insets) -> {
            if (Build.VERSION.SDK_INT >= 28 && insets.getDisplayCutout() != null) {
                android.view.DisplayCutout cutout = insets.getDisplayCutout();
                setPadding(cutout.getSafeInsetLeft(), cutout.getSafeInsetTop(),
                        cutout.getSafeInsetRight(), cutout.getSafeInsetBottom());
            } else setPadding(0, 0, 0, 0);
            return insets;
        });
        setVisibility(View.GONE);
    }

    void showLookSettings(boolean touch) {
        String title = touch ? "Touch aim sensitivity" : "Right-stick sensitivity";
        int minimum = touch ? 25 : 50;
        LinearLayout settings = new LinearLayout(getContext());
        settings.setOrientation(LinearLayout.VERTICAL);
        settings.setPadding(dp(24), dp(8), dp(24), 0);
        TextView label = new TextView(getContext());
        SeekBar slider = new SeekBar(getContext());
        slider.setMax(touch ? 55 : 30);
        slider.setProgress(((touch ? touchSensitivity : lookSensitivity) - minimum) / 5);
        slider.setContentDescription(title);
        label.setText((minimum + 5 * slider.getProgress()) + "%");
        slider.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                label.setText((minimum + 5 * progress) + "%");
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        settings.addView(label);
        settings.addView(slider);
        new AlertDialog.Builder(getContext())
                .setTitle(title)
                .setView(settings)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Apply", (dialog, which) -> {
                    int sensitivity = minimum + 5 * slider.getProgress();
                    if (touch) touchSensitivity = sensitivity;
                    else lookSensitivity = sensitivity;
                    controlPreferences.edit().putInt(touch ? "touch-aim-sensitivity" : "right-stick-sensitivity", sensitivity).apply();
                })
                .show();
    }

    private final class HudControl {
        final String id, label;
        final View view;
        final int x, y;
        final boolean right, bottom;
        float horizontal, vertical;
        int size;

        HudControl(String id, String label, View view, int x, int y, boolean right, boolean bottom) {
            this.id = "hud-" + id;
            this.label = label;
            this.view = view;
            this.x = dp(x);
            this.y = dp(y);
            this.right = right;
            this.bottom = bottom;
            read();
        }

        void read() {
            horizontal = controlPreferences.getFloat(id + "-x", Float.NaN);
            vertical = controlPreferences.getFloat(id + "-y", Float.NaN);
            size = Math.max(60, Math.min(160, controlPreferences.getInt(id + "-size", 100)));
        }

        void save(SharedPreferences.Editor prefs) {
            if (Float.isNaN(horizontal)) prefs.remove(id + "-x").remove(id + "-y");
            else prefs.putFloat(id + "-x", horizontal).putFloat(id + "-y", vertical);
            prefs.putInt(id + "-size", size);
        }
    }

    private void addControl(String id, String label, View view, int width, int height,
                            int x, int y, boolean right, boolean bottom) {
        hud.add(new HudControl(id, label, view, x, y, right, bottom));
        addView(view, new LayoutParams(dp(width), dp(height)));
    }

    private int hudWidth() { return Math.max(1, getWidth() - getPaddingLeft() - getPaddingRight()); }
    private int hudHeight() { return Math.max(1, getHeight() - getPaddingTop() - getPaddingBottom()); }

    @Override protected void onLayout(boolean changed, int l, int t, int r, int b) {
        super.onLayout(changed, l, t, r, b);
        for (HudControl control : hud) {
            View view = control.view;
            int width = view.getMeasuredWidth(), height = view.getMeasuredHeight();
            float fit = control.bottom ? 1 : Math.min(1, hudWidth() / (float)dp(424));
            float scale = Math.min(control.size / 100f * fit,
                    Math.min(hudWidth() / (float)width, hudHeight() / (float)height));
            float halfWidth = width * scale / 2, halfHeight = height * scale / 2;
            float x = control.right ? hudWidth() - control.x - width / 2f : (control.x + width / 2f) * fit;
            float y = control.bottom ? hudHeight() - control.y - height / 2f - Math.min(dp(100), hudHeight() * .14f)
                    : control.y + height / 2f;
            if (Float.isFinite(control.horizontal) && Float.isFinite(control.vertical)) {
                x = control.horizontal * hudWidth();
                y = control.vertical * hudHeight();
            }
            int left = Math.round(getPaddingLeft() + Math.max(halfWidth, Math.min(hudWidth() - halfWidth, x)) - width / 2f);
            int top = Math.round(getPaddingTop() + Math.max(halfHeight, Math.min(hudHeight() - halfHeight, y)) - height / 2f);
            view.layout(left, top, left + width, top + height);
            view.setScaleX(scale);
            view.setScaleY(scale);
        }
    }

    void showTouchLayout() {
        Dialog dialog = new Dialog(getContext(), android.R.style.Theme_Material_NoActionBar);
        InputOverlay preview = new InputOverlay(getContext(), null);
        preview.editing = true;
        preview.padMode.setVisibility(View.GONE);
        preview.look.setVisibility(View.GONE);
        preview.setVisibility(View.VISIBLE);
        preview.setBackgroundColor(0xff252a20);
        LinearLayout editor = new LinearLayout(getContext());
        editor.setOrientation(LinearLayout.VERTICAL);
        editor.setFitsSystemWindows(true);
        LinearLayout toolbar = new LinearLayout(getContext());
        toolbar.setOrientation(LinearLayout.VERTICAL);
        toolbar.setPadding(dp(12), dp(6), dp(12), dp(6));
        preview.selectedLabel = new TextView(getContext());
        preview.selectedLabel.setText("Drag any control. Tap one to resize it.");
        toolbar.addView(preview.selectedLabel);
        preview.resizeControl = new SeekBar(getContext());
        preview.resizeControl.setContentDescription("Selected control size");
        preview.resizeControl.setMax(100);
        preview.resizeControl.setEnabled(false);
        preview.resizeControl.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                if (fromUser && preview.selectedControl != null) {
                    preview.selectedControl.size = 60 + progress;
                    preview.selectControl(preview.selectedControl);
                    preview.requestLayout();
                }
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        toolbar.addView(preview.resizeControl);
        LinearLayout buttons = new LinearLayout(getContext());
        Button reset = new Button(getContext());
        reset.setText("Reset");
        reset.setOnClickListener(v -> {
            for (HudControl control : preview.hud) {
                control.horizontal = control.vertical = Float.NaN;
                control.size = 100;
            }
            preview.selectControl(preview.selectedControl);
            preview.requestLayout();
        });
        Button cancel = new Button(getContext());
        cancel.setText("Cancel");
        cancel.setOnClickListener(v -> dialog.dismiss());
        Button save = new Button(getContext());
        save.setText("Save");
        save.setOnClickListener(v -> {
            SharedPreferences.Editor prefs = controlPreferences.edit();
            for (HudControl control : preview.hud) control.save(prefs);
            prefs.apply();
            for (HudControl control : hud) control.read();
            requestLayout();
            dialog.dismiss();
        });
        buttons.addView(reset);
        buttons.addView(cancel);
        buttons.addView(save);
        toolbar.addView(buttons);
        editor.addView(toolbar);
        editor.addView(preview, new LinearLayout.LayoutParams(-1, 0, 1));
        dialog.setContentView(editor);
        dialog.show();
        dialog.getWindow().setLayout(-1, -1);
    }

    private void selectControl(HudControl control) {
        selectedControl = control;
        for (HudControl item : hud) item.view.setAlpha(control == null || item == control ? 1f : .6f);
        if (control == null) return;
        selectedLabel.setText(control.label + " · " + control.size + "%");
        resizeControl.setEnabled(true);
        resizeControl.setProgress(control.size - 60);
    }

    @Override public boolean dispatchTouchEvent(MotionEvent event) {
        if (!editing) return super.dispatchTouchEvent(event);
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            editPointer = -1;
            for (int i = hud.size() - 1; i >= 0; i--) {
                HudControl control = hud.get(i);
                View view = control.view;
                float cx = view.getLeft() + view.getWidth() / 2f, cy = view.getTop() + view.getHeight() / 2f;
                if (Math.abs(event.getX() - cx) > view.getWidth() * view.getScaleX() / 2
                        || Math.abs(event.getY() - cy) > view.getHeight() * view.getScaleY() / 2) continue;
                selectControl(control);
                editPointer = event.getPointerId(0);
                editX = event.getX() - cx;
                editY = event.getY() - cy;
                break;
            }
        }
        int index = event.findPointerIndex(editPointer);
        if (action == MotionEvent.ACTION_MOVE && index >= 0) {
            selectedControl.horizontal = Math.max(0, Math.min(1, (event.getX(index) - editX - getPaddingLeft()) / hudWidth()));
            selectedControl.vertical = Math.max(0, Math.min(1, (event.getY(index) - editY - getPaddingTop()) / hudHeight()));
            requestLayout();
        }
        if (action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_UP
                || (action == MotionEvent.ACTION_POINTER_UP && index == event.getActionIndex())) editPointer = -1;
        return true;
    }

    static void setImmersive(Activity activity, boolean playing) {
        View decor = activity.getWindow().getDecorView();
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = activity.getWindow().getInsetsController();
            if (controller == null) return;
            if (playing) {
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            } else {
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_DEFAULT);
                controller.show(WindowInsets.Type.systemBars());
            }
        } else {
            decor.setSystemUiVisibility(playing ? View.SYSTEM_UI_FLAG_FULLSCREEN
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY : 0);
        }
    }

    private final class MovementPad extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int pointer = -1;
        private float x, y, originX, originY;
        private long downTime;
        private boolean aimedTap;

        MovementPad(Context context) {
            super(context);
            setContentDescription("Movement stick");
        }

        @Override protected void onDraw(Canvas canvas) {
            float radius = Math.min(dp(48), Math.min(getWidth(), getHeight()) * .5f);
            float cx = pointer < 0 ? getWidth() * .5f : Math.max(radius, Math.min(getWidth() - radius, originX));
            float cy = pointer < 0 ? getHeight() * .5f : Math.max(radius, Math.min(getHeight() - radius, originY));
            paint.setColor(0x68131812);
            canvas.drawCircle(cx, cy, radius - dp(2), paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(2));
            paint.setColor(0xb4d9e9a4);
            canvas.drawCircle(cx, cy, radius - dp(3), paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(pointer < 0 ? 0x88a8ba7d : 0xe0d9e9a4);
            canvas.drawCircle(cx + x * radius * .60f, cy + y * radius * .60f, radius * .29f, paint);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (!active) return false;
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                pointer = event.getPointerId(0);
                originX = event.getX();
                originY = event.getY();
                downTime = event.getEventTime();
                aimedTap = touchAimHeld() && !ProbeActivity.gameMenuActive();
            }
            int index = event.findPointerIndex(pointer);
            if (aimedTap && index >= 0) {
                if (Math.hypot(event.getX(index) - originX, event.getY(index) - originY) >= dp(10)) aimedTap = false;
                for (int i = 0; aimedTap && i < event.getHistorySize(); i++) {
                    if (Math.hypot(event.getHistoricalX(index, i) - originX,
                            event.getHistoricalY(index, i) - originY) >= dp(10)) aimedTap = false;
                }
            }
            if (action == MotionEvent.ACTION_CANCEL || action == MotionEvent.ACTION_UP ||
                    (action == MotionEvent.ACTION_POINTER_UP && event.getPointerId(event.getActionIndex()) == pointer)) {
                if (action != MotionEvent.ACTION_CANCEL && index >= 0 && aimedTap
                        && event.getEventTime() - downTime <= 220 && touchAimHeld()
                        && !ProbeActivity.gameMenuActive()) pulseFire();
                reset();
                return true;
            }
            if (index >= 0) {
                float radius = Math.max(1, dp(42));
                x = (event.getX(index) - originX) / radius;
                y = (event.getY(index) - originY) / radius;
                float length = Math.max(1, (float)Math.hypot(x, y));
                x /= length;
                y /= length;
                movement(x, y, TOUCH);
                invalidate();
            }
            return true;
        }

        void reset() {
            pointer = -1;
            aimedTap = false;
            x = y = 0;
            movement(0, 0, TOUCH);
            invalidate();
        }
    }

    private void movement(float x, float y, int source) {
        key(FORWARD, y < -.34f, source);
        key(BACK, y > .34f, source);
        key(LEFT, x < -.34f, source);
        key(RIGHT, x > .34f, source);
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private Button button(String title, int width) {
        Button button = new Button(getContext());
        button.setText(title);
        button.setTextSize(12);
        button.setAllCaps(false);
        button.setTextColor(Color.rgb(234, 245, 202));
        button.setFocusable(false);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setPadding(0, 0, 0, 0);
        GradientDrawable shape = new GradientDrawable();
        shape.setCornerRadius(dp(15));
        shape.setColor(0xa0192018);
        shape.setStroke(dp(1), 0xb4d9e9a4);
        button.setBackground(new android.graphics.drawable.RippleDrawable(
                ColorStateList.valueOf(0x60d9e9a4), shape, null));
        button.setSoundEffectsEnabled(false);
        LinearLayout.LayoutParams size = new LinearLayout.LayoutParams(dp(width), dp(48));
        size.setMargins(dp(3), dp(3), dp(3), dp(3));
        button.setLayoutParams(size);
        return button;
    }
    private void key(int code, boolean down, int source) {
        int previous = keySources[code];
        int next = down ? previous | source : previous & ~source;
        keySources[code] = next;
        if ((previous != 0) != (next != 0)) ProbeActivity.inputKey(code, next != 0);
    }
    private void mouse(int button, boolean down, int source) {
        int previous = mouseSources[button];
        int next = down ? previous | source : previous & ~source;
        mouseSources[button] = next;
        if (button == 1 && !touchAimHeld()) ((MovementPad)movementPad).aimedTap = false;
        if ((previous != 0) != (next != 0)) ProbeActivity.inputButton(button, next != 0);
    }
    private boolean touchAimHeld() { return (mouseSources[1] & TOUCH) != 0; }

    private void pulseFire() {
        mouse(0, true, TAP);
        handler.removeCallbacks(endTap);
        handler.postDelayed(endTap, 80);
    }

    private void touchLook(float dx, float dy) {
        float scale = ProbeActivity.gameMenuActive() ? 1f : touchSensitivity / 100f;
        float x = dx * scale + aimRemainderX, y = dy * scale + aimRemainderY;
        int stepX = Math.round(x), stepY = Math.round(y);
        aimRemainderX = x - stepX;
        aimRemainderY = y - stepY;
        if (stepX != 0 || stepY != 0) ProbeActivity.inputMotion(stepX, stepY);
    }
    private Button keyButton(String title, int code, int width) {
        Button button = button(title, width);
        button.setOnTouchListener((view, event) -> {
            if (!active) return false;
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) { key(code, true, TOUCH); view.setPressed(true); }
            if (event.getActionMasked() == MotionEvent.ACTION_UP || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
                key(code, false, TOUCH); view.setPressed(false);
                if (event.getActionMasked() == MotionEvent.ACTION_UP) view.performClick();
            }
            return true;
        });
        return button;
    }
    private Button mouseButton(String title, int index, int width) {
        Button button = button(title, width);
        button.setOnTouchListener(new OnTouchListener() {
            private int pointer = -1;
            private float x, y;

            @Override public boolean onTouch(View view, MotionEvent event) {
                if (!active) return false;
                int action = event.getActionMasked();
                if (action == MotionEvent.ACTION_DOWN) {
                    pointer = event.getPointerId(0);
                    x = event.getX();
                    y = event.getY();
                    mouse(index, true, TOUCH);
                    view.setPressed(true);
                }
                int position = event.findPointerIndex(pointer);
                if (action == MotionEvent.ACTION_MOVE && position >= 0) {
                    float nextX = event.getX(position), nextY = event.getY(position);
                    if (lookPointer < 0 && (mouseSources[index] & TOUCH) != 0 && (index == 1 || !touchAimHeld()))
                        touchLook((nextX - x) * view.getScaleX(), (nextY - y) * view.getScaleY());
                    x = nextX;
                    y = nextY;
                }
                if (action == MotionEvent.ACTION_CANCEL || ((action == MotionEvent.ACTION_UP ||
                        action == MotionEvent.ACTION_POINTER_UP) && position == event.getActionIndex())) {
                    pointer = -1;
                    mouse(index, false, TOUCH);
                    view.setPressed(false);
                    if (action == MotionEvent.ACTION_UP) view.performClick();
                }
                return true;
            }
        });
        return button;
    }
    private void mouseState(MotionEvent event) {
        int buttons = event.getButtonState();
        mouse(0, (buttons & MotionEvent.BUTTON_PRIMARY) != 0, MOUSE);
        mouse(1, (buttons & MotionEvent.BUTTON_SECONDARY) != 0, MOUSE);
        mouse(2, (buttons & MotionEvent.BUTTON_TERTIARY) != 0, MOUSE);
    }
    private boolean mouseMotion(MotionEvent event) {
        if (!active || (event.getSource() & android.view.InputDevice.SOURCE_MOUSE) != android.view.InputDevice.SOURCE_MOUSE) return false;
        mouseState(event);
        if (event.getActionMasked() == MotionEvent.ACTION_SCROLL) {
            ProbeActivity.inputWheel(Math.round(event.getAxisValue(MotionEvent.AXIS_VSCROLL) * 120)); return true;
        }
        if (event.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE || event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            if (!Float.isNaN(hoverX)) ProbeActivity.inputMotion(Math.round(event.getX() - hoverX), Math.round(event.getY() - hoverY));
            hoverX = event.getX(); hoverY = event.getY();
        }
        return true;
    }
    private boolean touch(MotionEvent event) {
        if (!active) return false;
        if ((event.getSource() & android.view.InputDevice.SOURCE_MOUSE) == android.view.InputDevice.SOURCE_MOUSE) {
            mouseState(event); return mouseMotion(event);
        }
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            lookPointer = event.getPointerId(0);
            lastX = event.getX();
            lastY = event.getY();
            distance = 0;
            menuTap = ProbeActivity.gameMenuActive();
        }
        int index = event.findPointerIndex(lookPointer);
        if (action == MotionEvent.ACTION_MOVE && index >= 0) {
            float dx = event.getX(index) - lastX, dy = event.getY(index) - lastY;
            touchLook(dx, dy);
            distance += Math.abs(dx) + Math.abs(dy);
            lastX = event.getX(index);
            lastY = event.getY(index);
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            if (index >= 0 && index == event.getActionIndex()) {
                lookPointer = -1;
                if (menuTap && distance < dp(12) && ProbeActivity.gameMenuActive()) {
                    pulseFire();
                }
            }
        }
        if (action == MotionEvent.ACTION_CANCEL) lookPointer = -1;
        return true;
    }

    boolean keyboard(KeyEvent event) {
        if (!active || (event.getAction() != KeyEvent.ACTION_DOWN && event.getAction() != KeyEvent.ACTION_UP)) return false;
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        InputDevice device = event.getDevice();
        int androidKey = event.getKeyCode();
        boolean direction = androidKey >= KeyEvent.KEYCODE_DPAD_UP && androidKey <= KeyEvent.KEYCODE_DPAD_CENTER;
        if (event.isFromSource(InputDevice.SOURCE_GAMEPAD) || event.isFromSource(InputDevice.SOURCE_JOYSTICK)
                || (direction && device != null && (device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                || device.supportsSource(InputDevice.SOURCE_JOYSTICK)))
                || KeyEvent.isGamepadButton(androidKey)) return gamepadButton(event, down);
        int code = keyCode(event);
        if (code == 0) return false;
        key(code, down, KEYBOARD);
        return true;
    }

    private boolean gamepadButton(KeyEvent event, boolean down) {
        if (!gamepadEnabled) return false;
        int mask;
        switch (event.getKeyCode()) {
            case KeyEvent.KEYCODE_DPAD_UP: mask = 0x0001; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: mask = 0x0002; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: mask = 0x0004; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: mask = 0x0008; break;
            case KeyEvent.KEYCODE_BUTTON_START: mask = 0x0010; break;
            case KeyEvent.KEYCODE_BUTTON_SELECT: mask = 0x0020; break;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: mask = 0x0040; break;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: mask = 0x0080; break;
            case KeyEvent.KEYCODE_BUTTON_L1: mask = 0x0100; break;
            case KeyEvent.KEYCODE_BUTTON_R1: mask = 0x0200; break;
            case KeyEvent.KEYCODE_BUTTON_A:
            case KeyEvent.KEYCODE_DPAD_CENTER: mask = 0x1000; break;
            case KeyEvent.KEYCODE_BUTTON_B: mask = 0x2000; break;
            case KeyEvent.KEYCODE_BUTTON_X: mask = 0x4000; break;
            case KeyEvent.KEYCODE_BUTTON_Y: mask = 0x8000; break;
            case KeyEvent.KEYCODE_BUTTON_L2:
            case KeyEvent.KEYCODE_BUTTON_R2:
                if (selectGamepad(event.getDeviceId(), down)) {
                    if (event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_L2) leftTriggerButton = down;
                    else rightTriggerButton = down;
                    publishGamepad();
                }
                return true;
            default: return false;
        }
        if (selectGamepad(event.getDeviceId(), down)) {
            gamepadButtons = down ? gamepadButtons | mask : gamepadButtons & ~mask;
            publishGamepad();
        }
        return true;
    }

    private boolean selectGamepad(int id, boolean allowSwitch) {
        if (gamepadId != id) {
            if (!allowSwitch) return false;
            clearGamepad();
            gamepadId = id;
        }
        return true;
    }

    private static float axis(MotionEvent event, InputDevice device, int axis) {
        float value = event.getAxisValue(axis);
        InputDevice.MotionRange range = device == null ? null : device.getMotionRange(axis, event.getSource());
        if (range != null && Math.abs(value) <= range.getFlat()) return 0;
        return Math.max(-1, Math.min(1, value));
    }

    private static int stick(float value) {
        return Math.round(value * (value < 0 ? 32768 : 32767));
    }

    static int lookAxis(float value, int sensitivity) {
        // Scale travel beyond the standard XInput right-thumb dead zone.
        float deadZone = 8689f / 32767f;
        float magnitude = Math.abs(value);
        if (sensitivity == 100 || magnitude <= deadZone) return stick(value);
        float adjusted = Math.min(1, deadZone + (magnitude - deadZone) * sensitivity / 100f);
        return stick(Math.copySign(adjusted, value));
    }

    boolean gamepadMotion(MotionEvent event) {
        if (!active || !gamepadEnabled || !event.isFromSource(InputDevice.SOURCE_JOYSTICK)
                || event.getActionMasked() != MotionEvent.ACTION_MOVE) return false;
        InputDevice device = event.getDevice();
        selectGamepad(event.getDeviceId(), true);
        leftX = stick(axis(event, device, MotionEvent.AXIS_X));
        leftY = stick(-axis(event, device, MotionEvent.AXIS_Y));
        float hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X), hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y);
        gamepadHat = (hatX < -.5f ? 4 : hatX > .5f ? 8 : 0) | (hatY < -.5f ? 1 : hatY > .5f ? 2 : 0);
        leftTrigger = Math.round(255 * Math.max(trigger(event, device, MotionEvent.AXIS_LTRIGGER),
                trigger(event, device, MotionEvent.AXIS_BRAKE)));
        rightTrigger = Math.round(255 * Math.max(trigger(event, device, MotionEvent.AXIS_RTRIGGER),
                trigger(event, device, MotionEvent.AXIS_GAS)));
        boolean useRx = device != null && device.getMotionRange(MotionEvent.AXIS_RX, event.getSource()) != null
                && device.getMotionRange(MotionEvent.AXIS_RY, event.getSource()) != null
                && (device.getMotionRange(MotionEvent.AXIS_Z, event.getSource()) == null
                || device.getMotionRange(MotionEvent.AXIS_RZ, event.getSource()) == null);
        rightX = lookAxis(axis(event, device, useRx ? MotionEvent.AXIS_RX : MotionEvent.AXIS_Z), lookSensitivity);
        rightY = lookAxis(-axis(event, device, useRx ? MotionEvent.AXIS_RY : MotionEvent.AXIS_RZ), lookSensitivity);
        publishGamepad();
        return true;
    }

    private static float trigger(MotionEvent event, InputDevice device, int axis) {
        float value = event.getAxisValue(axis);
        InputDevice.MotionRange range = device == null ? null : device.getMotionRange(axis, event.getSource());
        if (range != null && range.getRange() > 0) value = (value - range.getMin()) / range.getRange();
        return Math.max(0, Math.min(1, value));
    }

    private void publishGamepad() {
        ProbeActivity.inputGamepad(gamepadId != NO_GAMEPAD, gamepadButtons | gamepadHat,
                leftTriggerButton ? 255 : leftTrigger, rightTriggerButton ? 255 : rightTrigger,
                leftX, leftY, rightX, rightY);
    }

    void refreshGamepad() {
        if (!active || !gamepadEnabled || gamepadId != NO_GAMEPAD || inputManager == null) return;
        for (int id : inputManager.getInputDeviceIds()) {
            InputDevice device = inputManager.getInputDevice(id);
            if (device != null && !device.isVirtual() && (device.supportsSource(InputDevice.SOURCE_GAMEPAD)
                    || device.supportsSource(InputDevice.SOURCE_JOYSTICK))) {
                gamepadId = id;
                publishGamepad();
                return;
            }
        }
    }

    private static int keyCode(KeyEvent event) {
        int key = event.getKeyCode();
        int[] letters = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
            0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
        if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z) return letters[key - KeyEvent.KEYCODE_A];
        if (key >= KeyEvent.KEYCODE_0 && key <= KeyEvent.KEYCODE_9) return key == KeyEvent.KEYCODE_0 ? 0x0b : key - KeyEvent.KEYCODE_1 + 2;
        switch (key) {
            case KeyEvent.KEYCODE_ESCAPE: return 1; case KeyEvent.KEYCODE_TAB: return 0x0f;
            case KeyEvent.KEYCODE_SPACE: return 0x39; case KeyEvent.KEYCODE_ENTER: return 0x1c;
            case KeyEvent.KEYCODE_DEL: return 0x0e; case KeyEvent.KEYCODE_SHIFT_LEFT: return 0x2a;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0x36; case KeyEvent.KEYCODE_CTRL_LEFT: return 0x1d;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0x9d; case KeyEvent.KEYCODE_ALT_LEFT: return 0x38;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xb8; case KeyEvent.KEYCODE_DPAD_UP: return 0xc8;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xd0; case KeyEvent.KEYCODE_DPAD_LEFT: return 0xcb;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xcd;
            default: return event.getScanCode() > 0 && event.getScanCode() < 89 ? event.getScanCode() : 0;
        }
    }
    void setActive(boolean value) {
        if (active == value) return;
        active = value;
        setVisibility(value && showTouch ? View.VISIBLE : View.GONE);
        if (inputManager != null) {
            if (value) inputManager.registerInputDeviceListener(this, handler);
            else inputManager.unregisterInputDeviceListener(this);
        }
        if (value) refreshGamepad();
        else releaseAll();
    }

    void setPlayMode(boolean controllerOnly) {
        releaseAll();
        showTouch = !controllerOnly;
        gamepadEnabled = controllerOnly;
        padMode.setVisibility(View.GONE);
        look.setVisibility(View.GONE);
        setVisibility(active && showTouch ? View.VISIBLE : View.GONE);
        refreshGamepad();
    }

    private void clearGamepad() {
        gamepadId = NO_GAMEPAD;
        gamepadButtons = gamepadHat = leftX = leftY = rightX = rightY = leftTrigger = rightTrigger = 0;
        leftTriggerButton = rightTriggerButton = false;
    }

    private void releaseGamepad() {
        if (gamepadId == NO_GAMEPAD) return;
        clearGamepad();
        publishGamepad();
    }

    void releaseAll() {
        handler.removeCallbacks(endTap);
        handler.removeCallbacks(endCamera);
        releaseGamepad();
        ((MovementPad)movementPad).reset();
        for (int i = 1; i < keySources.length; ++i) if (keySources[i] != 0) key(i, false, ~0);
        for (int i = 0; i < mouseSources.length; ++i) if (mouseSources[i] != 0) mouse(i, false, ~0);
        lookPointer = -1;
        aimRemainderX = aimRemainderY = 0;
        hoverX = hoverY = Float.NaN;
    }

    @Override public void onInputDeviceAdded(int id) { refreshGamepad(); }
    @Override public void onInputDeviceChanged(int id) {
        if (id == gamepadId) releaseGamepad();
        refreshGamepad();
    }
    @Override public void onInputDeviceRemoved(int id) {
        if (id == gamepadId) releaseGamepad();
        refreshGamepad();
    }
    @Override protected void onDetachedFromWindow() {
        setActive(false);
        super.onDetachedFromWindow();
    }
}
