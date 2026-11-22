package dev.pdisp

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID

/** A PC the user connects to over the network. */
data class Pc(val id: String, val name: String, val host: String, val lastUsed: Long)

data class Settings(
    val keepScreenOn: Boolean = true,
    /** Full panel brightness while the picture is showing (this app's window only; the system setting is untouched). */
    val fullBrightness: Boolean = true,
    val palmRejection: Boolean = true,
    val showStats: Boolean = false,
    /** Highest refresh rate (Hz) this device offers the PC; 0 = everything the display and decoder can do. */
    val maxRefreshHz: Int = 0,
)

/** The saved PCs and settings (SharedPreferences), and the device's pairing files. */
class Store(private val ctx: Context) {
    private val prefs = ctx.getSharedPreferences("pdisp", Context.MODE_PRIVATE)

    init { // the single address the first versions remembered
        val old = ctx.getSharedPreferences("MainActivity", Context.MODE_PRIVATE).getString("host", null)
        if (old != null && !prefs.contains("pcs")) {
            if (old.isNotBlank()) upsert(old.trim(), null)
            ctx.getSharedPreferences("MainActivity", Context.MODE_PRIVATE).edit().remove("host").apply()
        }
    }

    fun pcs(): List<Pc> {
        val arr = try { JSONArray(prefs.getString("pcs", "[]")) } catch (e: Exception) { JSONArray() }
        val list = (0 until arr.length()).mapNotNull {
            val o = arr.optJSONObject(it) ?: return@mapNotNull null
            Pc(o.optString("id"), o.optString("name"), o.optString("host"), o.optLong("lastUsed"))
        }.filter { it.id.isNotEmpty() && it.host.isNotEmpty() }
        return list.sortedByDescending { it.lastUsed }
    }

    private fun save(list: List<Pc>) {
        val arr = JSONArray()
        for (p in list) arr.put(JSONObject().put("id", p.id).put("name", p.name).put("host", p.host).put("lastUsed", p.lastUsed))
        prefs.edit().putString("pcs", arr.toString()).apply()
    }

    /** Adds the PC, or updates the one with the same address. Returns it. */
    fun upsert(host: String, name: String?): Pc {
        val list = pcs().toMutableList()
        val i = list.indexOfFirst { it.host.equals(host, ignoreCase = true) }
        val pc = if (i >= 0) list[i].copy(name = if (name.isNullOrBlank()) list[i].name else name)
        else Pc(UUID.randomUUID().toString(), if (name.isNullOrBlank()) host else name.trim(), host, 0)
        if (i >= 0) list[i] = pc else list.add(pc)
        save(list)
        return pc
    }

    fun used(id: String) = save(pcs().map { if (it.id == id) it.copy(lastUsed = System.currentTimeMillis()) else it })
    fun rename(id: String, name: String) = save(pcs().map { if (it.id == id && name.isNotBlank()) it.copy(name = name.trim()) else it })
    fun remove(id: String) = save(pcs().filter { it.id != id })

    var settings: Settings
        get() = Settings(
            prefs.getBoolean("keepScreenOn", true), prefs.getBoolean("fullBrightness", true), prefs.getBoolean("palmRejection", true),
            prefs.getBoolean("showStats", false), prefs.getInt("maxRefreshHz", 0),
        )
        set(v) {
            prefs.edit().putBoolean("keepScreenOn", v.keepScreenOn).putBoolean("fullBrightness", v.fullBrightness).putBoolean("palmRejection", v.palmRejection)
                .putBoolean("showStats", v.showStats).putInt("maxRefreshHz", v.maxRefreshHz).apply()
        }

    /** Forgets this device's key and every PC it trusted: the next connection pairs again (the PC may also have to forget this device: pdisp keys). */
    fun forgetPairing() {
        for (name in listOf("identity", "known_pcs")) File(ctx.filesDir, name).delete()
    }
}
