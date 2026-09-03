package fr.wozt.capture2cloud

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder

/**
 * Keeps the stream a foreground concern while the window is not.
 *
 * Android does not merely stop drawing a backgrounded app: it moves the
 * process into a bucket where the scheduler and the network stack both
 * treat it as expendable. The socket keeps filling while the app is
 * given less and less time to empty it, and what comes back on return
 * is not a live stream but a queue of stale pictures being worked
 * through -- which is exactly what the picture looked like.
 *
 * A foreground service is the documented way to say "this process is
 * still doing the thing the user asked for". It costs a notification,
 * which is also the honest thing to show: something IS still running.
 */
class StreamService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val manager = getSystemService(NotificationManager::class.java)
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL, "streaming", NotificationManager.IMPORTANCE_LOW)
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )
        val note: Notification = Notification.Builder(this, CHANNEL)
            .setContentTitle("capture2cloud")
            .setContentText("streaming in the background")
            .setSmallIcon(R.mipmap.ic_launcher)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        startForeground(ID, note)
        return START_NOT_STICKY
    }

    companion object {
        private const val CHANNEL = "stream"
        private const val ID = 1

        fun start(context: Context) {
            context.startForegroundService(Intent(context, StreamService::class.java))
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, StreamService::class.java))
        }
    }
}
