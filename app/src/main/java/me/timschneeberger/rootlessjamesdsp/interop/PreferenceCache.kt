package me.timschneeberger.rootlessjamesdsp.interop

import android.content.Context
import android.content.SharedPreferences
import androidx.annotation.StringRes
import me.timschneeberger.rootlessjamesdsp.flavor.CrashlyticsImpl
import kotlin.reflect.KClass

class PreferenceCache(val context: Context) {
    val changedNamespaces = ArrayList<String>()
    val cache: HashMap<String, Any> = hashMapOf()
    var selectedNamespace: String? = null

    fun select(namespace: String) {
        selectedNamespace = namespace
    }

    fun clear() {
        try {
            cache.clear()
        }
        catch (_: Exception) {}
    }

    @Suppress("UNCHECKED_CAST")
    fun <T : Any> get(@StringRes nameRes: Int, default: T, type: KClass<T>): T =
        get(context.getString(nameRes), default, type)

    // String-key variant (same cache / change-detection semantics) for cards
    // whose keys are generated rather than @string resources (the multiband
    // compressor's 206 per-split / per-band keys, dsp_mbcomp_preferences.xml).
    @Suppress("UNCHECKED_CAST")
    fun <T : Any> get(name: String, default: T, type: KClass<T>): T {
        if(selectedNamespace == null)
            throw IllegalStateException("No active namespace selected")

        val current = uncachedGet(context, selectedNamespace!!, name, default, type)
        val unchanged = cache.containsKey(name) && cache[name] == current
        if(!unchanged && !changedNamespaces.contains(selectedNamespace)) {
            selectedNamespace?.let {
                changedNamespaces.add(it)
            }
        }

        CrashlyticsImpl.setCustomKey("dsp_$name", current.toString())
        cache[name] = current as Any
        return current
    }

    inline fun <reified T : Any> get(@StringRes nameRes: Int, default: T) =
        get(nameRes, default, T::class)

    inline fun <reified T : Any> get(name: String, default: T) =
        get(name, default, T::class)

    fun markChangesAsCommitted() {
        changedNamespaces.clear()
    }

    companion object {
        @Suppress("DEPRECATION")
        fun getPreferences(
            context: Context,
            namespace: String,
        ): SharedPreferences {
            return context.getSharedPreferences(namespace, Context.MODE_MULTI_PROCESS)
        }

        @Suppress("UNCHECKED_CAST")
        fun <T : Any> uncachedGet(
            context: Context,
            namespace: String,
            @StringRes nameRes: Int,
            default: T,
            type: KClass<T>
        ): T = uncachedGet(context, namespace, context.getString(nameRes), default, type)

        @Suppress("UNCHECKED_CAST")
        fun <T : Any> uncachedGet(
            context: Context,
            namespace: String,
            name: String,
            default: T,
            type: KClass<T>
        ): T {
            val prefs = getPreferences(context, namespace)
            val current: T = when(type) {
                Boolean::class -> prefs.getBoolean(name, default as Boolean) as T
                String::class -> prefs.getString(name, default as String) as T
                Int::class -> prefs.getInt(name, default as Int) as T
                Float::class -> prefs.getFloat(name, default as Float) as T
                else -> throw IllegalArgumentException("Unknown type")
            }
            return current
        }

        inline fun <reified T : Any> uncachedGet(
            context: Context,
            namespace: String,
            @StringRes nameRes: Int,
            default: T
        ) = uncachedGet(context, namespace, nameRes, default, T::class)
    }
}