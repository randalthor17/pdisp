package dev.pdisp

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBarsPadding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.clickable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** A column no wider than a comfortable reading width, centred: the same screens serve a phone and a tablet. */
@Composable
private fun Page(modifier: Modifier = Modifier, content: @Composable () -> Unit) {
    Surface(modifier = Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.background) {
        Box(Modifier.fillMaxSize().systemBarsPadding(), contentAlignment = Alignment.TopCenter) {
            Column(modifier.widthIn(max = 640.dp).fillMaxWidth().padding(horizontal = 20.dp)) { content() }
        }
    }
}

private fun ago(ms: Long): String {
    if (ms == 0L) return "never connected"
    val s = (System.currentTimeMillis() - ms) / 1000
    return when {
        s < 60 -> "connected just now"
        s < 3600 -> "connected ${s / 60} min ago"
        s < 86400 -> "connected ${s / 3600} h ago"
        else -> "connected ${s / 86400} days ago"
    }
}

// ---- the start screen

@Composable
fun HomeScreen(
    c: SessionController, pcs: List<Pc>, onConnect: (Pc) -> Unit, onAdd: (String, String) -> Unit,
    onRename: (Pc, String) -> Unit, onForget: (Pc) -> Unit, onSettings: () -> Unit,
) {
    var adding by remember { mutableStateOf(false) }
    var renaming by remember { mutableStateOf<Pc?>(null) }
    Page(Modifier.verticalScroll(rememberScrollState())) {
        Row(Modifier.fillMaxWidth().padding(top = 16.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("pdisp", style = MaterialTheme.typography.headlineLarge, color = MaterialTheme.colorScheme.primary)
                Text("A second monitor for your Linux PC", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            IconButton(onClick = onSettings) { Icon(Icons.Default.Settings, contentDescription = "Settings") }
        }
        Spacer(Modifier.height(8.dp))

        c.problem?.let { p ->
            Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer), modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp)) {
                    Text(p.title, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onErrorContainer)
                    p.hint?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onErrorContainer, modifier = Modifier.padding(top = 4.dp)) }
                    TextButton(onClick = { c.problem = null }, modifier = Modifier.align(Alignment.End)) { Text("Dismiss") }
                }
            }
            Spacer(Modifier.height(12.dp))
        }

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                Text("USB cable", style = MaterialTheme.typography.titleMedium)
                Text(
                    c.usbHint ?: "Plug this device into the PC. If pdisp runs there, the monitor appears by itself. Nothing to set up on this screen.",
                    style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 4.dp),
                )
            }
        }
        Spacer(Modifier.height(20.dp))

        Text("Wi-Fi", style = MaterialTheme.typography.titleMedium, modifier = Modifier.padding(bottom = 8.dp))
        if (pcs.isEmpty()) {
            Card(modifier = Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text("No PC yet", style = MaterialTheme.typography.titleSmall)
                    Text("1. Install and start pdisp on the PC (docs/install.md).", style = MaterialTheme.typography.bodyMedium)
                    Text("2. Find the PC's address (for example 192.168.0.20).", style = MaterialTheme.typography.bodyMedium)
                    Text("3. Tap Add a PC below and type it. The first time, the PC asks you to approve this device.", style = MaterialTheme.typography.bodyMedium)
                }
            }
        }
        for (pc in pcs) PcCard(pc, onClick = { onConnect(pc) }, onRename = { renaming = pc }, onForget = { onForget(pc) })
        Spacer(Modifier.height(8.dp))
        Button(onClick = { adding = true }, modifier = Modifier.fillMaxWidth().height(52.dp)) {
            Icon(Icons.Default.Add, contentDescription = null, modifier = Modifier.size(20.dp))
            Spacer(Modifier.size(8.dp))
            Text("Add a PC")
        }
        Spacer(Modifier.height(24.dp))
    }
    if (adding) AddPcDialog(onDismiss = { adding = false }, onAdd = { host, name -> adding = false; onAdd(host, name) })
    renaming?.let { pc -> RenameDialog(pc, onDismiss = { renaming = null }, onRename = { renaming = null; onRename(pc, it) }) }
}

@Composable
private fun PcCard(pc: Pc, onClick: () -> Unit, onRename: () -> Unit, onForget: () -> Unit) {
    var menu by remember { mutableStateOf(false) }
    Card(modifier = Modifier.fillMaxWidth().padding(bottom = 8.dp).clickable(onClick = onClick)) {
        Row(Modifier.padding(start = 16.dp, top = 8.dp, bottom = 8.dp, end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(pc.name, style = MaterialTheme.typography.titleMedium)
                Text(if (pc.name == pc.host) ago(pc.lastUsed) else "${pc.host}  ·  ${ago(pc.lastUsed)}", style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Box {
                IconButton(onClick = { menu = true }) { Icon(Icons.Default.MoreVert, contentDescription = "More") }
                DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
                    DropdownMenuItem(text = { Text("Rename") }, onClick = { menu = false; onRename() })
                    DropdownMenuItem(text = { Text("Remove from the list") }, onClick = { menu = false; onForget() })
                }
            }
        }
    }
}

@Composable
private fun AddPcDialog(onDismiss: () -> Unit, onAdd: (String, String) -> Unit) {
    var host by remember { mutableStateOf("") }
    var name by remember { mutableStateOf("") }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() } // the keyboard opens here, not when the app starts
    val go = { if (host.isNotBlank()) onAdd(host.trim(), name.trim()) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Add a PC") },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = host, onValueChange = { host = it }, label = { Text("PC address") }, placeholder = { Text("192.168.0.20") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Next), modifier = Modifier.focusRequester(focus),
                )
                OutlinedTextField(
                    value = name, onValueChange = { name = it }, label = { Text("Name (optional)") }, singleLine = true,
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Go), keyboardActions = KeyboardActions(onGo = { go() }),
                )
            }
        },
        confirmButton = { TextButton(onClick = go, enabled = host.isNotBlank()) { Text("Connect") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

@Composable
private fun RenameDialog(pc: Pc, onDismiss: () -> Unit, onRename: (String) -> Unit) {
    var name by remember { mutableStateOf(pc.name) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Rename") },
        text = { OutlinedTextField(value = name, onValueChange = { name = it }, singleLine = true) },
        confirmButton = { TextButton(onClick = { onRename(name) }, enabled = name.isNotBlank()) { Text("Save") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Cancel") } },
    )
}

// ---- connecting and pairing

@Composable
fun ConnectingScreen(c: SessionController, onCancel: () -> Unit) {
    val target = c.target
    val name = if (target is Target.Host) target.pc.name else "the PC"
    val text = when (c.stage) {
        0 -> "Connecting to $name"
        1 -> "Connected over USB, waiting for the PC"
        2 -> "Securing the connection"
        3 -> "Waiting for the PC to create the monitor"
        5 -> "Starting the video"
        SessionController.STAGE_RECONNECTING -> "Lost the PC. Reconnecting (attempt ${c.attempts})"
        else -> c.rawStatus.ifBlank { "Connecting" }
    }
    Page {
        Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
            CircularProgressIndicator()
            Spacer(Modifier.height(24.dp))
            Text(text, style = MaterialTheme.typography.titleMedium, textAlign = TextAlign.Center)
            c.usbHint?.let { Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center, modifier = Modifier.padding(top = 8.dp)) }
            Box(Modifier.height(88.dp).padding(top = 12.dp), contentAlignment = Alignment.TopCenter) { // space is reserved: the Cancel button must not move when the hint appears
                if (c.slow) Text(
                if (target is Target.Usb) "The PC is not answering over the cable. On the PC, pdispd must be running with the USB option, and not be busy with another device."
                else "The PC is slow to answer. Is pdispd running on it, and not busy with another device?",
                style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.tertiary, textAlign = TextAlign.Center, modifier = Modifier.padding(top = 12.dp),
                )
            }
            Spacer(Modifier.height(32.dp))
            OutlinedButton(onClick = onCancel) { Text("Cancel") }
        }
    }
}

@Composable
fun PairingScreen(c: SessionController, onCancel: () -> Unit) {
    val target = c.target
    val name = if (target is Target.Host) target.pc.name else "your PC"
    val code = c.pairCode ?: ""
    Page {
        Column(Modifier.fillMaxSize(), horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center) {
            Text("Approve this device", style = MaterialTheme.typography.headlineSmall, textAlign = TextAlign.Center)
            Text("A notification on $name asks whether this device may use it as a monitor. The code there must be the same as this one.",
                style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, textAlign = TextAlign.Center,
                modifier = Modifier.padding(top = 8.dp, bottom = 24.dp))
            Text(if (code.length == 6) code.substring(0, 3) + " " + code.substring(3) else code, fontSize = 56.sp, fontFamily = FontFamily.Monospace,
                color = MaterialTheme.colorScheme.primary)
            Spacer(Modifier.height(8.dp))
            Text("If the codes differ, choose Deny on the PC.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(32.dp))
            OutlinedButton(onClick = onCancel) { Text("Cancel") }
        }
    }
}

// ---- settings

@Composable
fun SettingsScreen(c: SessionController, settings: Settings, onChange: (Settings) -> Unit, onForgetPairing: () -> Unit, onBack: () -> Unit) {
    var confirm by remember { mutableStateOf(false) }
    val rates = remember { c.displayRatesHz() }
    Page(Modifier.verticalScroll(rememberScrollState())) {
        Row(Modifier.fillMaxWidth().padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onBack) { Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Back") }
            Text("Settings", style = MaterialTheme.typography.titleLarge)
        }
        SwitchRow("Keep the screen on", "While connected, so the monitor does not go dark.", settings.keepScreenOn) { onChange(settings.copy(keepScreenOn = it)) }
        SwitchRow("Palm rejection", "Ignore large contacts, such as a hand resting on the screen while writing with a pen.", settings.palmRejection) { onChange(settings.copy(palmRejection = it)) }
        SwitchRow("Show statistics", "Frame rate and lost frames in a corner while streaming.", settings.showStats) { onChange(settings.copy(showStats = it)) }
        HorizontalDivider(Modifier.padding(vertical = 12.dp))
        Text("Highest refresh rate", style = MaterialTheme.typography.titleMedium)
        Text("What this device offers the PC. The PC chooses from these, and by default stays at 60 Hz whatever you pick here; raise its limit with pdispd -R.",
            style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(vertical = 4.dp))
        Row(Modifier.padding(top = 8.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(selected = settings.maxRefreshHz == 0, onClick = { onChange(settings.copy(maxRefreshHz = 0)) }, label = { Text("Auto") })
            for (hz in rates) FilterChip(selected = settings.maxRefreshHz == hz, onClick = { onChange(settings.copy(maxRefreshHz = hz)) }, label = { Text("$hz Hz") })
        }
        HorizontalDivider(Modifier.padding(vertical = 12.dp))
        Text("Pairing", style = MaterialTheme.typography.titleMedium)
        Text("This device proves who it is to a PC with a key. Forgetting it makes every PC treat this device as new: you approve it again, and the PC's old entry can be removed with pdisp-keys.",
            style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(vertical = 4.dp))
        OutlinedButton(onClick = { confirm = true }, modifier = Modifier.padding(top = 8.dp)) { Text("Forget this device's pairing") }
        HorizontalDivider(Modifier.padding(vertical = 12.dp))
        Text("About", style = MaterialTheme.typography.titleMedium)
        Text("pdisp ${BuildConfigInfo.version}. This device shows up on the PC as \"${android.os.Build.MODEL}\".", style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.padding(top = 4.dp, bottom = 24.dp))
    }
    if (confirm) AlertDialog(
        onDismissRequest = { confirm = false },
        title = { Text("Forget the pairing?") },
        text = { Text("The next connection to any PC has to be approved again.") },
        confirmButton = { TextButton(onClick = { confirm = false; onForgetPairing() }) { Text("Forget") } },
        dismissButton = { TextButton(onClick = { confirm = false }) { Text("Cancel") } },
    )
}

@Composable
private fun SwitchRow(title: String, subtitle: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().clickable { onChange(!checked) }.padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).padding(end = 12.dp)) {
            Text(title, style = MaterialTheme.typography.bodyLarge)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Switch(checked = checked, onCheckedChange = onChange)
    }
}

// ---- while streaming

/** A small, non-interactive readout in a corner: touches go through it to the PC. */
@Composable
fun StatsOverlay(s: Stats?) {
    if (s == null) return
    Box(Modifier.fillMaxSize().systemBarsPadding().padding(8.dp), contentAlignment = Alignment.TopEnd) {
        Surface(color = MaterialTheme.colorScheme.surface.copy(alpha = 0.7f), shape = MaterialTheme.shapes.small) {
            Text("${s.width}×${s.height}  ${fpsText(s.fps)}  lost ${s.lostFrames}", style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp))
        }
    }
}

/** Opened with Back while streaming: the stream owns every touch, so the controls live behind the system's own gesture. */
@Composable
fun StreamMenu(c: SessionController, settings: Settings, onChange: (Settings) -> Unit, onDismiss: () -> Unit, onDisconnect: () -> Unit) {
    val s = c.stats
    val target = c.target
    val who = if (target is Target.Host) target.pc.name else "USB"
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Connected to $who") },
        text = {
            Column {
                if (s != null) Text("${s.width}×${s.height}, ${fpsText(s.fps)}", style = MaterialTheme.typography.bodyMedium)
                Spacer(Modifier.height(8.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text("Show statistics", Modifier.weight(1f))
                    Switch(checked = settings.showStats, onCheckedChange = { onChange(settings.copy(showStats = it)) })
                }
                Text("Press Back again to return to the stream.", style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 8.dp))
            }
        },
        confirmButton = { TextButton(onClick = onDisconnect) { Text("Disconnect") } },
        dismissButton = { TextButton(onClick = onDismiss) { Text("Close") } },
    )
}

/** A screen that does not change sends no new pictures: that is not a stalled stream. */
private fun fpsText(fps: Float) = if (fps < 0.5f) "picture still" else "%.0f fps".format(fps)

object BuildConfigInfo {
    val version: String get() = "0.1"
}
