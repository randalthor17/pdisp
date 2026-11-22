package dev.pdisp

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.util.Log
import androidx.compose.runtime.mutableStateListOf
import java.net.Inet4Address

/** A PC that announced itself on the local network (pdispd's mDNS service, docs/discovery.md). */
data class FoundPc(val serviceName: String, val name: String, val host: String, val port: Int, val fingerprint: String?) {
    /** How the PC is stored and connected to: the address, with the port only when it is not the default. */
    val address: String get() = if (port == 47110) host else "$host:$port"
}

/** Looks for `_pdisp._tcp` while the home screen is showing. The system's NsdManager does the mDNS work; names are resolved one at a time. */
class Discovery(ctx: Context) {
    private val nsd = ctx.applicationContext.getSystemService(Context.NSD_SERVICE) as NsdManager
    val found = mutableStateListOf<FoundPc>()
    private var listener: NsdManager.DiscoveryListener? = null
    private val queue = ArrayDeque<NsdServiceInfo>()
    private var resolving = false

    fun start() {
        if (listener != null) return
        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) {}
            override fun onDiscoveryStopped(serviceType: String) {}
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) { Log.w("pdisp", "mDNS discovery failed: $errorCode"); listener = null }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
            override fun onServiceFound(info: NsdServiceInfo) { synchronized(queue) { queue.addLast(info) }; next() }
            override fun onServiceLost(info: NsdServiceInfo) { post { found.removeAll { it.serviceName == info.serviceName } } }
        }
        listener = l
        try { nsd.discoverServices("_pdisp._tcp", NsdManager.PROTOCOL_DNS_SD, l) } catch (e: Exception) { Log.w("pdisp", "mDNS: $e"); listener = null }
    }

    fun stop() {
        val l = listener ?: return
        listener = null
        try { nsd.stopServiceDiscovery(l) } catch (_: Exception) {}
        synchronized(queue) { queue.clear() }
        post { found.clear() }
    }

    @Suppress("DEPRECATION") // resolveService works on every version we support; its replacement needs API 34
    private fun next() {
        val info = synchronized(queue) { if (resolving || queue.isEmpty()) return; resolving = true; queue.removeFirst() }
        nsd.resolveService(info, object : NsdManager.ResolveListener {
            override fun onResolveFailed(serviceInfo: NsdServiceInfo, errorCode: Int) { done() }
            override fun onServiceResolved(r: NsdServiceInfo) {
                val addr = r.host as? Inet4Address // the PC announces IPv4 only
                if (addr != null) {
                    val fp = r.attributes["fp"]?.toString(Charsets.UTF_8)
                    val pc = FoundPc(r.serviceName, r.serviceName.removePrefix("pdisp on "), addr.hostAddress ?: "", r.port, fp)
                    post { found.removeAll { it.serviceName == pc.serviceName }; found.add(pc) }
                }
                done()
            }
        })
    }

    private fun done() { synchronized(queue) { resolving = false }; next() }
    private fun post(f: () -> Unit) = android.os.Handler(android.os.Looper.getMainLooper()).post(f)
}
