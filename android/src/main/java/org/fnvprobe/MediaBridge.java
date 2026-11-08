package org.fnvprobe;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

/** Android media objects live exclusively on one Looper, including callbacks. */
public final class MediaBridge {
    private static final HandlerThread thread = new HandlerThread("fnv-music");
    private static final Handler handler;
    private static final SparseArray<Track> tracks = new SparseArray<>();
    static { thread.start(); handler = new Handler(thread.getLooper()); }
    private static final class Track {
        MediaPlayer player;
        int state, volume, balance;
        boolean complete, prepared;
        void gain() {
            if (player == null) return;
            float gain = volume <= -10000 ? 0 : (float)Math.pow(10, volume / 2000.0);
            player.setVolume(gain * (balance > 0 ? (float)Math.pow(10, -balance/2000.0) : 1),
                    gain * (balance < 0 ? (float)Math.pow(10, balance/2000.0) : 1));
        }
        void close() { if (player != null) { player.release(); player=null; } prepared=false; state=0; complete=false; }
    }
    public static double dispatch(int id, int operation, String path, double value) {
        try {
            if (Looper.myLooper() == thread.getLooper()) return command(id,operation,path,value);
            FutureTask<Double> task = new FutureTask<>(() -> command(id,operation,path,value));
            if (!handler.post(task)) return -1;
            try { return task.get(10,TimeUnit.SECONDS); }
            catch (Exception failure) { task.cancel(false); throw failure; }
        } catch (Exception failure) { Log.e("FNVMedia","Music operation " + operation + " failed",failure); return -1; }
    }
    private static double command(int id,int operation,String path,double value) throws Exception {
        if(operation==0){for(int i=0;i<tracks.size();++i)tracks.valueAt(i).close();tracks.clear();return 0;}
        if(operation==1){if(tracks.get(id)!=null)return -1;tracks.put(id,new Track());return 0;}
        Track track=tracks.get(id);if(track==null)return -1;
        if(operation==2){track.close();tracks.remove(id);return 0;}
        if(operation==3){
            track.close();MediaPlayer player=new MediaPlayer();track.player=player;
            try {
                player.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
                player.setOnCompletionListener(ignored -> {track.complete=true;track.state=0;});
                player.setOnErrorListener((ignored,what,extra) -> {track.state=0;track.prepared=false;Log.e("FNVMedia","Decoder error " + what + "/" + extra);return true;});
                try(FileInputStream input=new FileInputStream(path)){player.setDataSource(input.getFD());}
                player.prepare();track.prepared=true;track.gain();return 0;
            } catch(Exception failure){track.close();throw failure;}
        }
        if(operation==8){track.volume=(int)value;track.gain();return 0;}
        if(operation==9){track.balance=(int)value;track.gain();return 0;}
        if(!track.prepared)return -1;
        switch(operation){
            case 4: track.complete=false;track.player.start();track.state=2;return 0;
            case 5: if(track.state==2)track.player.pause();track.state=1;return 0;
            case 6: if(track.state==2)track.player.pause();track.player.seekTo(0);track.state=0;track.complete=false;return 0;
            case 7: return track.state;
            case 10: return track.player.getDuration()/1000.0;
            case 11: return track.player.getCurrentPosition()/1000.0;
            case 12: track.player.seekTo(Math.max(0,(long)(value*1000)),MediaPlayer.SEEK_CLOSEST);track.complete=false;return 0;
            case 13: return track.complete?1:0;
            default: return -1;
        }
    }
    public static int selfTest(String root) {
        final int id=Integer.MAX_VALUE;File file=new File(root,".probe-music-test.wav");
        try {
            ByteBuffer wav=ByteBuffer.allocate(44+16000).order(ByteOrder.LITTLE_ENDIAN);
            wav.put(new byte[]{'R','I','F','F'}).putInt(16036).put(new byte[]{'W','A','V','E','f','m','t',' '})
                .putInt(16).putShort((short)1).putShort((short)1).putInt(8000).putInt(16000)
                .putShort((short)2).putShort((short)16).put(new byte[]{'d','a','t','a'}).putInt(16000);
            try(FileOutputStream out=new FileOutputStream(file)){out.write(wav.array());}
            if(dispatch(id,1,null,0)!=0||dispatch(id,8,null,-10000)!=0||dispatch(id,3,file.getAbsolutePath(),0)!=0)return 1;
            if(Math.abs(dispatch(id,10,null,0)-1)>.05)return 2;
            if(dispatch(id,4,null,0)!=0)return 3;
            long deadline=android.os.SystemClock.uptimeMillis()+2000;
            while(dispatch(id,11,null,0)<=0&&android.os.SystemClock.uptimeMillis()<deadline)Thread.sleep(20);
            if(dispatch(id,11,null,0)<=0||dispatch(id,7,null,0)!=2)return 4;
            if(dispatch(id,5,null,0)!=0||dispatch(id,7,null,0)!=1)return 5;
            if(dispatch(id,12,null,.5)!=0)return 6;
            deadline=android.os.SystemClock.uptimeMillis()+1000;
            while(Math.abs(dispatch(id,11,null,0)-.5)>.15&&android.os.SystemClock.uptimeMillis()<deadline)Thread.sleep(20);
            if(Math.abs(dispatch(id,11,null,0)-.5)>.15)return 7;
            if(dispatch(id,6,null,0)!=0||dispatch(id,7,null,0)!=0)return 8;
            return 0;
        } catch(Exception failure){Log.e("FNVMedia","Music selftest failed",failure);return 9;}
        finally {dispatch(id,2,null,0);if(file.exists()&&!file.delete())Log.w("FNVMedia","Test fixture cleanup failed");}
    }
}
