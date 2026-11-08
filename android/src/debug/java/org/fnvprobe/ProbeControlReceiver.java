package org.fnvprobe;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Debug build only; shell DUMP permission controls game input, not Android input. */
public final class ProbeControlReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context,Intent intent) {
        try {
            System.loadLibrary("fnvprobe");
            String operation=intent.getStringExtra("operation");
            if("frame".equals(operation))ProbeActivity.requestGameFrame();
            else if("pointer".equals(operation))ProbeActivity.reportGamePointer();
            else if("stop".equals(operation))ProbeActivity.requestTraceStop();
            else if("move".equals(operation)) {
                int x=intent.getIntExtra("x",0),y=intent.getIntExtra("y",0);
                if(Math.abs((long)x)>4096||Math.abs((long)y)>4096)throw new IllegalArgumentException("Motion exceeds test range.");
                ProbeActivity.inputMotion(x,y);
            } else if("key".equals(operation)) {
                int key=intent.getIntExtra("code",-1);if(key<0||key>255)throw new IllegalArgumentException("Invalid key.");
                ProbeActivity.inputKey(key,intent.getBooleanExtra("down",false));
            } else if("button".equals(operation)) {
                int button=intent.getIntExtra("code",-1);if(button<0||button>7)throw new IllegalArgumentException("Invalid button.");
                ProbeActivity.inputButton(button,intent.getBooleanExtra("down",false));
            } else throw new IllegalArgumentException("Unknown game test operation.");
            android.util.Log.i("FNVInput","Debug operation "+operation+" pid="+android.os.Process.myPid());
            setResultCode(Activity.RESULT_OK);setResultData("Queued game test operation.");
        } catch(Exception | LinkageError error){setResultCode(Activity.RESULT_CANCELED);setResultData(error.toString());}
    }
}
