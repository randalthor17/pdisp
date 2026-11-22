package dev.pdisp

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

private val Dark = darkColorScheme(
    primary = Color(0xFF6FD8C6), onPrimary = Color(0xFF00382F), primaryContainer = Color(0xFF005046), onPrimaryContainer = Color(0xFF8CF5E1),
    secondary = Color(0xFFB1CCC5), background = Color(0xFF0E1513), surface = Color(0xFF0E1513),
    surfaceVariant = Color(0xFF3F4946), onSurface = Color(0xFFDDE4E1), onSurfaceVariant = Color(0xFFBEC9C5),
)
private val Light = lightColorScheme(
    primary = Color(0xFF006B5E), onPrimary = Color.White, primaryContainer = Color(0xFF8CF5E1), onPrimaryContainer = Color(0xFF00201B),
    secondary = Color(0xFF4A635E), background = Color(0xFFF4FBF8), surface = Color(0xFFF4FBF8),
    surfaceVariant = Color(0xFFDAE5E1), onSurface = Color(0xFF161D1B), onSurfaceVariant = Color(0xFF3F4946),
)

/** Material 3, following the system's light or dark setting, with the user's wallpaper colours on Android 12 and newer. */
@Composable
fun PdispTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val ctx = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= 31 -> if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        dark -> Dark
        else -> Light
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
