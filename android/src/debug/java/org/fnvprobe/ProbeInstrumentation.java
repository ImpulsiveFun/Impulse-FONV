package org.fnvprobe;

import android.app.Activity;
import android.app.Instrumentation;
import android.os.Bundle;
import android.os.SystemClock;
import android.media.Image;
import android.media.ImageReader;
import android.graphics.PixelFormat;
import android.view.ContextThemeWrapper;
import android.view.MotionEvent;
import android.view.SurfaceView;
import java.nio.ByteBuffer;
import android.util.Log;
import java.io.File;
import java.nio.file.Files;
import java.security.MessageDigest;

/** Debug-only device runner; takes no arbitrary file path or executable code. */
public final class ProbeInstrumentation extends Instrumentation {
    private int duration=90;
    private boolean inputScript;
    private boolean showMain;
    private String launcherMode;
    @Override public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        if (arguments != null) {
            duration = Integer.parseInt(arguments.getString("duration", "90"));
            inputScript = Boolean.parseBoolean(arguments.getString("script", "false"));
            showMain = Boolean.parseBoolean(arguments.getString("showMain", "false"));
            launcherMode = arguments.getString("launcher");
        }
        start();
    }

    private String checkSurface() throws Exception {
        try (ImageReader reader = ImageReader.newInstance(16, 16, PixelFormat.RGBA_8888, 2)) {
            ProbeActivity.configureSurface(reader.getSurface());
            try {
                String nativeCheck = ProbeActivity.runSurfaceCheck();
                if (!nativeCheck.startsWith("PASS:")) throw new IllegalStateException(nativeCheck);
                Image image = null;
                long end = SystemClock.uptimeMillis() + 1000;
                while (image == null && SystemClock.uptimeMillis() < end) {
                    image = reader.acquireNextImage();
                    if (image == null) SystemClock.sleep(10);
                }
                if (image == null) throw new IllegalStateException("Android surface did not receive a frame.");
                try (Image captured = image) {
                    Image.Plane plane = captured.getPlanes()[0];
                    ByteBuffer pixels = plane.getBuffer();
                    int left = 8 * plane.getRowStride() + 4 * plane.getPixelStride();
                    int right = 8 * plane.getRowStride() + 12 * plane.getPixelStride();
                    if ((pixels.get(left) & 255) != 0 || (pixels.get(left + 1) & 255) != 0 ||
                        (pixels.get(left + 2) & 255) != 255 || (pixels.get(left + 3) & 255) != 255 ||
                        (pixels.get(right) & 255) != 255 || (pixels.get(right + 1) & 255) != 0 ||
                        (pixels.get(right + 2) & 255) != 0 || (pixels.get(right + 3) & 255) != 255)
                        throw new IllegalStateException("Android surface pixels differed from the rendered frame.");
                }
                return "PASS: Android Surface received the expected rendered pixels.";
            } finally { ProbeActivity.configureSurface(null); }
        }
    }
    private static void holdForward(InputOverlay overlay, long now) {
        android.view.View pad = overlay.movementPad;
        if (pad.getWidth() == 0 || pad.getHeight() == 0) pad.layout(0, 0, 128, 128);
        MotionEvent down = MotionEvent.obtain(now, now, MotionEvent.ACTION_DOWN,
                pad.getWidth() * .5f, pad.getHeight() * .5f, 0);
        MotionEvent move = MotionEvent.obtain(now, now + 1, MotionEvent.ACTION_MOVE,
                pad.getWidth() * .5f, pad.getHeight() * .10f, 0);
        try { pad.dispatchTouchEvent(down); pad.dispatchTouchEvent(move); }
        finally { down.recycle(); move.recycle(); }
    }

    private String checkGamepad() {
        ProbeActivity.runInputCheck(false);
        InputOverlay[] overlay = new InputOverlay[1];
        try {
            runOnMainSync(() -> {
                ContextThemeWrapper context = new ContextThemeWrapper(getTargetContext(),
                        android.R.style.Theme_Material_Light_NoActionBar);
                SurfaceView screen = new SurfaceView(context);
                overlay[0] = new InputOverlay(context, screen);
                overlay[0].setActive(true);
                long now = SystemClock.uptimeMillis();
                MotionEvent.PointerProperties pointer = new MotionEvent.PointerProperties();
                pointer.id = 0;
                MotionEvent.PointerCoords axes = new MotionEvent.PointerCoords();
                axes.setAxisValue(MotionEvent.AXIS_Y, -.5f);
                axes.setAxisValue(MotionEvent.AXIS_Z, -.5f);
                axes.setAxisValue(MotionEvent.AXIS_RTRIGGER, .5f);
                MotionEvent event = MotionEvent.obtain(now, now, MotionEvent.ACTION_MOVE, 1,
                        new MotionEvent.PointerProperties[]{pointer}, new MotionEvent.PointerCoords[]{axes},
                        0, 0, 1, 1, -1, 0, android.view.InputDevice.SOURCE_JOYSTICK, 0);
                try {
                    if (!overlay[0].gamepadMotion(event)) throw new IllegalStateException("Joystick event ignored.");
                } finally { event.recycle(); }
                android.view.KeyEvent face = new android.view.KeyEvent(now, now, android.view.KeyEvent.ACTION_DOWN,
                        android.view.KeyEvent.KEYCODE_BUTTON_A, 0, 0, -1, 0, 0, android.view.InputDevice.SOURCE_GAMEPAD);
                if (!overlay[0].keyboard(face)) throw new IllegalStateException("Face button ignored.");
            });
            int sensitivity = Math.max(50, Math.min(200, getTargetContext()
                    .getSharedPreferences("game-controls", android.content.Context.MODE_PRIVATE)
                    .getInt("right-stick-sensitivity", 100)));
            if (InputOverlay.lookAxis(-.5f, 100) != -16384 || InputOverlay.lookAxis(-.5f, 50) <= -16384
                    || InputOverlay.lookAxis(-.5f, 200) >= -16384 || InputOverlay.lookAxis(0, 200) != 0)
                throw new IllegalStateException("Right-stick sensitivity scaling failed.");
            if (!ProbeActivity.runGamepadCheck(true, InputOverlay.lookAxis(-.5f, sensitivity)))
                throw new IllegalStateException("Guest XInput did not receive analog axes, trigger and button.");
            runOnMainSync(() -> {
                overlay[0].setActive(false);
            });
            if (!ProbeActivity.runGamepadCheck(false, 0))
                throw new IllegalStateException("Guest XInput still reported a connected device after pause.");
            return "PASS: Guest XInput received sensitivity-scaled analog sticks, a partial trigger and a face button, then disconnected.";
        } finally {
            runOnMainSync(() -> { if (overlay[0] != null) overlay[0].setActive(false); });
            ProbeActivity.runInputCheck(false);
        }
    }

    private static float[] pointInOverlay(InputOverlay overlay, android.view.View view, float x, float y) {
        while (view != overlay) {
            x += view.getX();
            y += view.getY();
            view = (android.view.View)view.getParent();
        }
        return new float[]{x, y};
    }

    private static void dispatchTouch(InputOverlay overlay, long time, int action, float[]... points) {
        MotionEvent.PointerProperties[] ids = new MotionEvent.PointerProperties[points.length];
        MotionEvent.PointerCoords[] positions = new MotionEvent.PointerCoords[points.length];
        for (int i = 0; i < points.length; ++i) {
            ids[i] = new MotionEvent.PointerProperties();
            ids[i].id = i;
            ids[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            positions[i] = new MotionEvent.PointerCoords();
            positions[i].x = points[i][0];
            positions[i].y = points[i][1];
        }
        MotionEvent event = MotionEvent.obtain(time, time, action, points.length, ids, positions,
                0, 0, 1, 1, 0, 0, android.view.InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            if (!overlay.dispatchTouchEvent(event)) throw new IllegalStateException("Multi-touch event ignored.");
        } finally { event.recycle(); }
    }

    private String checkInput() {
        ProbeActivity.runInputCheck(false);
        InputOverlay[] overlay = new InputOverlay[1];
        try {
            runOnMainSync(() -> {
                ContextThemeWrapper context = new ContextThemeWrapper(getTargetContext(),
                        android.R.style.Theme_Material_Light_NoActionBar);
                InputOverlay controls = new InputOverlay(context, new SurfaceView(context));
                overlay[0] = controls;
                controls.setActive(true);
                float density = context.getResources().getDisplayMetrics().density;
                int width = Math.round(720 * density), height = Math.round(440 * density);
                int exact = android.view.View.MeasureSpec.EXACTLY;
                controls.measure(android.view.View.MeasureSpec.makeMeasureSpec(width, exact),
                        android.view.View.MeasureSpec.makeMeasureSpec(height, exact));
                controls.layout(0, 0, width, height);
                android.view.View pad = controls.movementPad, fire = controls.fireButton;
                float[] move = pointInOverlay(controls, pad, pad.getWidth() * .5f, pad.getHeight() * .5f);
                float[] forward = pointInOverlay(controls, pad, pad.getWidth() * .5f, pad.getHeight() * .1f);
                float[] shoot = pointInOverlay(controls, fire, fire.getWidth() * .5f, fire.getHeight() * .5f);
                float[] look = {width * .5f, height * .5f};
                long now = SystemClock.uptimeMillis();
                dispatchTouch(controls, now, MotionEvent.ACTION_DOWN, move);
                dispatchTouch(controls, now, MotionEvent.ACTION_POINTER_DOWN
                        | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), move, shoot);
                dispatchTouch(controls, now, MotionEvent.ACTION_POINTER_DOWN
                        | (2 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), move, shoot, look);
                dispatchTouch(controls, now, MotionEvent.ACTION_MOVE, forward, shoot,
                        new float[]{look[0] + 7, look[1] - 3});
            });
            String check = ProbeActivity.runInputCheck(true);
            if (!check.startsWith("PASS:")) throw new IllegalStateException(check);
            return "PASS: Simultaneous touch movement, fire and look reached native input.";
        } finally {
            runOnMainSync(() -> { if (overlay[0] != null) overlay[0].setActive(false); });
        }
    }

    private static void sendTouch(android.view.View view, long time, int action, float x, float y) {
        MotionEvent event = MotionEvent.obtain(time, time, action, x, y, 0);
        try {
            if (!view.dispatchTouchEvent(event)) throw new IllegalStateException("Touch ignored.");
        } finally { event.recycle(); }
    }

    private String checkTouchAim() {
        ProbeActivity.runInputCheck(false);
        runOnMainSync(() -> {
            ContextThemeWrapper context = new ContextThemeWrapper(getTargetContext(),
                    android.R.style.Theme_Material_Light_NoActionBar);
            android.content.SharedPreferences prefs = context.getSharedPreferences("game-controls", 0);
            boolean saved = prefs.contains("touch-aim-sensitivity");
            int previous = prefs.getInt("touch-aim-sensitivity", 100);
            prefs.edit().putInt("touch-aim-sensitivity", 150).apply();
            InputOverlay controls = new InputOverlay(context, null);
            try {
                controls.setActive(true);
                long now = SystemClock.uptimeMillis();
                sendTouch(controls.aimButton, now, MotionEvent.ACTION_DOWN, 10, 10);
                sendTouch(controls.aimButton, now + 1, MotionEvent.ACTION_MOVE, 14, 10);
                if (!ProbeActivity.checkTouchAim(false, false, 6))
                    throw new IllegalStateException("Touch aim sensitivity did not scale the drag.");
                holdForward(controls, now);
                if (!ProbeActivity.checkTouchAim(false, true, 0))
                    throw new IllegalStateException("An aimed stick drag did not move cleanly.");
                sendTouch(controls.movementPad, now + 20, MotionEvent.ACTION_UP, 10, 10);
                if (!ProbeActivity.checkTouchAim(false, false, 0))
                    throw new IllegalStateException("Releasing an aimed drag fired.");
                sendTouch(controls.movementPad, now + 30, MotionEvent.ACTION_DOWN, 40, 40);
                sendTouch(controls.movementPad, now + 70, MotionEvent.ACTION_UP, 40, 40);
                if (!ProbeActivity.checkTouchAim(true, false, 0))
                    throw new IllegalStateException("An aimed stick tap did not fire.");
            } finally {
                controls.setActive(false);
                android.content.SharedPreferences.Editor restore = prefs.edit();
                if (saved) restore.putInt("touch-aim-sensitivity", previous);
                else restore.remove("touch-aim-sensitivity");
                restore.apply();
            }
        });
        return "PASS: Touch sensitivity, aimed stick tap, and aimed stick drag.";
    }
    private String checkActivityControls() throws Exception {
        android.content.Intent intent = new android.content.Intent(getTargetContext(), ProbeActivity.class);
        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        ProbeActivity activity = (ProbeActivity) startActivitySync(intent);
        java.lang.reflect.Field controlsField=ProbeActivity.class.getDeclaredField("controls");
        java.lang.reflect.Field previewField=ProbeActivity.class.getDeclaredField("preview");
        java.lang.reflect.Field startupField=ProbeActivity.class.getDeclaredField("startup");
        controlsField.setAccessible(true);previewField.setAccessible(true);startupField.setAccessible(true);
        InputOverlay overlay=(InputOverlay)controlsField.get(activity);
        SurfaceView screen=(SurfaceView)previewField.get(activity);
        try {
            long deadline=SystemClock.uptimeMillis()+5000;boolean[] ready={false};
            while(!ready[0]&&SystemClock.uptimeMillis()<deadline){
                runOnMainSync(() -> {try{ready[0]=((android.widget.Button)startupField.get(activity)).isEnabled();}catch(IllegalAccessException error){throw new RuntimeException(error);}});
                if(!ready[0])SystemClock.sleep(20);
            }
            if(!ready[0])throw new IllegalStateException("Activity probe did not become ready.");
            ProbeActivity.runInputCheck(false);
            runOnMainSync(() -> {
                activity.setGameRunning(true);
                if(screen.getVisibility()!=android.view.View.VISIBLE||!screen.isShown()||!screen.getKeepScreenOn())
                    throw new IllegalStateException("Running game preview was not visible and awake.");
                if (activity.getActionBar() != null && activity.getActionBar().isShowing())
                    throw new IllegalStateException("The setup action bar stayed visible during play.");
                long now=SystemClock.uptimeMillis();
                MotionEvent down=MotionEvent.obtain(now,now,MotionEvent.ACTION_DOWN,10,10,0);
                try{holdForward(overlay, now);overlay.fireButton.dispatchTouchEvent(down);}finally{down.recycle();}
                callActivityOnPause(activity);
            });
            String released=ProbeActivity.runReleasedInputCheck();
            if(!released.startsWith("PASS:"))throw new IllegalStateException(released);
            runOnMainSync(() -> {
                callActivityOnResume(activity);
                long now=SystemClock.uptimeMillis();
                MotionEvent down=MotionEvent.obtain(now,now,MotionEvent.ACTION_DOWN,10,10,0);
                MotionEvent move=MotionEvent.obtain(now,now+1,MotionEvent.ACTION_MOVE,17,7,0);
                try{holdForward(overlay, now);overlay.fireButton.dispatchTouchEvent(down);overlay.fireButton.dispatchTouchEvent(move);}finally{down.recycle();move.recycle();}
            });
            String input=ProbeActivity.runInputCheck(true);
            if(!input.startsWith("PASS:"))throw new IllegalStateException(input);
            return "PASS: Normal activity shows the game, releases held input on pause, and accepts touch input after resume.";
        } finally {
            runOnMainSync(() -> {activity.setGameRunning(false);activity.finish();});
            waitForIdleSync();ProbeActivity.runInputCheck(false);
        }
    }
    private String traceGame(byte[] bytes, String digest) throws Exception {
        String root = ProbeActivity.prepareGameRoot(getTargetContext());
        if (!showMain) return ProbeActivity.traceStartup(bytes, digest, root, duration);

        android.content.Intent intent = new android.content.Intent(getTargetContext(), ProbeActivity.class);
        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        ProbeActivity activity = (ProbeActivity)startActivitySync(intent);
        try {
            java.lang.reflect.Field field = ProbeActivity.class.getDeclaredField("startup");
            field.setAccessible(true);
            android.widget.Button start = (android.widget.Button)field.get(activity);
            long deadline = SystemClock.uptimeMillis() + 5000;
            boolean[] ready = {false};
            while (!ready[0] && SystemClock.uptimeMillis() < deadline) {
                runOnMainSync(() -> ready[0] = start.isEnabled());
                if (!ready[0]) SystemClock.sleep(20);
            }
            if (!ready[0]) throw new IllegalStateException("Main game screen did not become ready.");
            runOnMainSync(() -> activity.setGameRunning(true));
            return ProbeActivity.traceStartup(bytes, digest, root, duration);
        } finally {
            runOnMainSync(() -> { activity.setGameRunning(false); activity.finish(); });
            waitForIdleSync();
        }
    }
    private static <T> T launcherField(ProbeActivity activity, String name, Class<T> type) throws Exception {
        java.lang.reflect.Field field = ProbeActivity.class.getDeclaredField(name);
        field.setAccessible(true);
        return type.cast(field.get(activity));
    }

    private String checkLauncher() throws Exception {
        boolean controller = "controller".equals(launcherMode);
        if (!controller && !"touch".equals(launcherMode)) throw new IllegalArgumentException("Unknown controls mode.");
        android.content.Intent intent = new android.content.Intent(getTargetContext(), ProbeActivity.class);
        intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
        ProbeActivity activity = (ProbeActivity)startActivitySync(intent);
        android.content.SharedPreferences prefs = activity.getPreferences(android.content.Context.MODE_PRIVATE);
        boolean hadPreference = prefs.contains("controller-only");
        boolean previous = prefs.getBoolean("controller-only", false);
        try {
            android.widget.Button play = launcherField(activity, "startup", android.widget.Button.class);
            android.widget.RadioGroup mode = launcherField(activity, "controlMode", android.widget.RadioGroup.class);
            InputOverlay controls = launcherField(activity, "controls", InputOverlay.class);
            SurfaceView screen = launcherField(activity, "preview", SurfaceView.class);
            boolean[] status = {false, false};
            long deadline = SystemClock.uptimeMillis() + 10000;
            while (!status[0] && SystemClock.uptimeMillis() < deadline) {
                runOnMainSync(() -> status[0] = play.isEnabled());
                if (!status[0]) SystemClock.sleep(20);
            }
            if (!status[0]) throw new IllegalStateException("Play did not become available.");
            runOnMainSync(() -> {
                mode.check(mode.getChildAt(controller ? 1 : 0).getId());
                play.performClick();
            });
            boolean observedGame = false;
            deadline = SystemClock.uptimeMillis() + Math.max(30, duration) * 1000L;
            while (SystemClock.uptimeMillis() < deadline) {
                runOnMainSync(() -> {
                    status[0] = screen.isShown();
                    status[1] = play.isEnabled();
                    if (status[0] && controls.isShown() == controller)
                        throw new IllegalStateException("Play showed the wrong controls.");
                });
                observedGame |= status[0];
                if (status[1]) {
                    if (!observedGame) throw new IllegalStateException("Play returned without a game surface.");
                    return "PASS: Play showed the expected " + launcherMode + " layout and returned to the launcher.";
                }
                SystemClock.sleep(250);
            }
            throw new IllegalStateException("Launcher test waiting for the game to close.");
        } finally {
            android.content.SharedPreferences.Editor restore = prefs.edit();
            if (hadPreference) restore.putBoolean("controller-only", previous);
            else restore.remove("controller-only");
            restore.commit();
            runOnMainSync(activity::finish);
        }
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            System.loadLibrary("fnvprobe");
            if (launcherMode != null) {
                result.putString("launcher_checks", checkLauncher());
                finish(Activity.RESULT_OK, result);
                return;
            }
            result.putString("activity_controls_checks", checkActivityControls());
            ProbeActivity.configureInputScript(inputScript);
            ProbeActivity.configureWindow(getTargetContext().getResources().getDisplayMetrics().widthPixels,
                    getTargetContext().getResources().getDisplayMetrics().heightPixels);
            String checks = ProbeActivity.runProbe();
            result.putString("register_checks", checks);
            if (!checks.startsWith("PASS:")) throw new IllegalStateException(checks);
            File executable = new File(getTargetContext().getFilesDir(), "probe/FalloutNV.exe");
            if (!executable.isFile() || executable.length() > 32L * 1024 * 1024)
                throw new IllegalArgumentException("Local executable unavailable: " + executable.getAbsolutePath()
                        + " (file=" + executable.isFile() + ", bytes=" + executable.length() + ")");
            byte[] bytes = Files.readAllBytes(executable.toPath());
            StringBuilder digest = new StringBuilder(64);
            for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes)) {
                digest.append(Character.forDigit((value >> 4) & 15, 16));
                digest.append(Character.forDigit(value & 15, 16));
            }
            String startup = traceGame(bytes, digest.toString());
            Log.i("FNVProbe", checks);
            Log.i("FNVProbe", startup);
            result.putString("startup", startup);
            if (!(startup.startsWith("Startup checks passed") || startup.startsWith("Memory,") ||
                  startup.startsWith("Startup stopped") || startup.startsWith("Startup reached a probe execution limit") || startup.startsWith("Guest requested") ||
                  startup.startsWith("Graphics stopped") || startup.startsWith("Guest software interrupt") || startup.startsWith("Startup entry returned")))
                throw new IllegalStateException(startup);
            result.putString("surface_checks", checkSurface());
            result.putString("input_bridge_checks", checkInput());
            result.putString("gamepad_checks", checkGamepad());
            result.putString("touch_aim_checks", checkTouchAim());
            finish(Activity.RESULT_OK, result);
        } catch (Exception | LinkageError failure) {
            Log.e("FNVProbe", "Automated device check failed", failure);
            result.putString("failure", failure.toString());
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
