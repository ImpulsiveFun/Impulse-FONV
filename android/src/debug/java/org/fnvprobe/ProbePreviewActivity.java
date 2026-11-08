package org.fnvprobe;

import android.app.Activity;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.FrameLayout;

/** Debug-only visible view of the running native game, without starting a second trace. */
public final class ProbePreviewActivity extends Activity {
    private InputOverlay controls;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);System.loadLibrary("fnvprobe");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root=new FrameLayout(this);SurfaceView screen=new SurfaceView(this);
        root.addView(screen,new FrameLayout.LayoutParams(-1,-1));
        controls=new InputOverlay(this,screen);root.addView(controls,new FrameLayout.LayoutParams(-1,-1));
        controls.setActive(true);
        screen.getHolder().addCallback(new SurfaceHolder.Callback(){
            @Override public void surfaceCreated(SurfaceHolder holder){ProbeActivity.configureSurface(holder.getSurface());}
            @Override public void surfaceChanged(SurfaceHolder holder,int format,int width,int height){ProbeActivity.configureSurface(holder.getSurface());}
            @Override public void surfaceDestroyed(SurfaceHolder holder){ProbeActivity.configureSurface(null);}
        });
        setContentView(root);screen.requestFocus();
        InputOverlay.setImmersive(this, true);
    }
    @Override protected void onResume(){super.onResume();if(controls!=null)controls.setActive(true);}
    @Override protected void onPause(){if(controls!=null)controls.setActive(false);super.onPause();}
    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        return controls != null && controls.gamepadMotion(event) || super.dispatchGenericMotionEvent(event);
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) {
            InputOverlay.setImmersive(this, true);
            if (controls != null) controls.refreshGamepad();
        }
        else if (controls != null) controls.releaseAll();
    }
    @Override public boolean dispatchKeyEvent(KeyEvent event){return controls!=null&&controls.keyboard(event)||super.dispatchKeyEvent(event);}
}
