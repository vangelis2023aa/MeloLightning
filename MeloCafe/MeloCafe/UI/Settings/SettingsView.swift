//
//  SettingsView.swift
//  MeloCafe
//
//  Created by Stossy11 on 7/4/2026.
//


import SwiftUI
import UniformTypeIdentifiers

extension CemuConfigWrapper {
    func cast<T>(_ value: Any) -> T? {
        value as? T
    }
}

extension Binding {
    func map<U>(
        get: @escaping (Value) -> U,
        set: @escaping (U) -> Value
    ) -> Binding<U> {
        Binding<U>(
            get: { get(self.wrappedValue) },
            set: { self.wrappedValue = set($0) }
        )
    }
}

struct NavigationStack<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        if #available(iOS 16, *) {
            SwiftUI.NavigationStack(root: content)
        } else {
            NavigationView(content: content)
                .navigationViewStyle(.stack)
        }
    }
}

struct AppIconPosition: Identifiable {
    var id: String { creator }
    var creator: String
    var icons: [AppIcon]
}

struct AppIcon: Identifiable {
    var id: String
    var name: String
    var def: Bool = false
}


struct SettingsView: View {
    @ObservedObject private var controllerManager: ControllerManager = .shared
    @StateObject private var configManager: ConfigManager = .shared
    @EnvironmentObject private var gameManager: GamesManager
    
    @AppStorage("cardType") var cardTypeRawValue: String = CardType.list.rawValue
    var cardType: Binding<CardType> {
        .init {
            CardType(rawValue: cardTypeRawValue) ?? .card
        } set: { type in
            cardTypeRawValue = type.rawValue
        }
    }
    
    @AppStorage("breakpoint") var stikDebugbreakpoint = false
    
    @AppStorage("showSwapButton") var showSwapButton: Bool = true
    @AppStorage("screenLayout") private var screenLayout = ScreenLayout.initialValue
    
    @State private var showingEmulatedDevices = false
    
    var body: some View {
        NavigationStack {
            Form {
                Section {
                    if checkAppEntitlement("get-task-allow") {
                        Picker("CPU Mode", selection: configManager.interpreter) {
                            ForEach(CPUMode.allCases, id: \.self) {
                                Text($0.string)
                            }
                        }
                        .pickerStyle(.menu)
                    } else {
                        Picker("CPU Mode", selection: .constant(CPUMode.interpreter)) {
                            ForEach(CPUMode.allCases, id: \.self) {
                                Text($0.string)
                            }
                        }
                        .pickerStyle(.menu)
                        .disabled(true)
                    }
                } header: {
                    Text("CPU")
                }
                
                Section("App") {
                    NavigationLink("App Icon Switcher") {
                        AppIconSwitcher()
                    }
                    
                    Picker("Library View", selection: cardType) {
                        ForEach(CardType.allCases, id: \.self) {
                            Text($0.displayName)
                        }
                    }
                    .pickerStyle(.menu)
                }
                
                
                Section("General") {
                    Picker("Console Language", selection: configManager.consoleLanguage) {
                        ForEach(ConsoleLanguage.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    .pickerStyle(.menu)
                    
                    
                    HStack {
                        Text("Screen Layout")
                        
                        Button {
                            AppAlerts.showSyncAlert(title: "Screen Layout", message: screenLayout.description)
                        } label: {
                            Image(systemName: "info.circle")
                        }
                        .foregroundStyle(.secondary)
                        
                        Spacer()
                        
                        Picker("", selection: $screenLayout) {
                            ForEach(ScreenLayout.allCases, id: \.self) { layout in
                                Text(layout.string).tag(layout)
                            }
                        }
                        .pickerStyle(.menu)
                    }

                    if screenLayout == .singleScreen {
                        Toggle("Show Swap Button (TV <-> Pad)", isOn: $showSwapButton)
                    }
                    
                    Toggle("Disable Screensaver", isOn: configManager.disableScreensaver)
                    Toggle("Play Boot Sound", isOn: configManager.playBootSound)
                }
                
                Section("Account") {
                    NavigationLink {
                        AccountSettingsView()
                    } label: {
                        HStack {
                            Text("Account settings")
                            Spacer()
                            Text(configManager.activeAccount?.displayName ?? "No account selected")
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                
                Section("Controllers") {
                    ForEach(controllerManager.controllers) { entry in
                        ControllerRow(entry: entry)
                            .contextMenu {
                                ForEach(ControllerType.allCases) { type in
                                    if !type.name.isEmpty {
                                        Button {
                                            controllerManager.setControllerType(id: entry.id, to: type)
                                        } label: {
                                            if entry.controllerType == type {
                                                Label(type.name, systemImage: "checkmark")
                                            } else {
                                                Text(type.name)
                                            }
                                        }
                                        .disabled(!controllerManager.canSelectType(type, for: entry.id))
                                    }
                                }
                            }
                    }
                    .onMove { source, destination in
                        controllerManager.move(from: source, to: destination)
                    }
                    .onDelete { offsets in
                        let ids = offsets.map { controllerManager.controllers[$0].id }
                        for id in ids { controllerManager.remove(id: id) }
                    }
                    
                    let cont = controllerManager.missingControllers()
                    
                    if !cont.isEmpty {
                        Menu {
                            ForEach(cont) { entry in
                                Button {
                                    controllerManager.addFromAll(id: entry.id)
                                } label: {
                                    Text(entry.name)
                                }
                                .disabled(!controllerManager.canAdd(id: entry.id))
                            }
                        } label: {
                            Label("Controllers", systemImage: "chevron.down")
                        }
                    }
                    
                }
                .environment(\.editMode, .constant(.active))
                
                Section("Graphics") {
                    Picker("Renderer", selection: configManager.renderer) {
                        ForEach(Renderer.allCases, id: \.self) {
                            if !$0.string.isEmpty {
                                Text($0.string)
                            }
                        }
                    }
                    
                    Toggle("VSync", isOn: configManager.vsync)
                    Toggle("GX2 DrawDone Sync", isOn: configManager.gx2DrawDoneSync)
                    Toggle("Render Upside Down", isOn: configManager.renderUpsideDown)
                    Toggle("Async Shader Compile", isOn: configManager.asyncCompile)
                    Toggle("Shader Cache", isOn: configManager.precompiledShaders)
                    
                    Picker("Upscale Filter", selection: configManager.upscaleFilter) {
                        ForEach(UpscalingFilter.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Picker("Downscale Filter", selection: configManager.downscaleFilter) {
                        ForEach(UpscalingFilter.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Picker("Fullscreen Scaling", selection: configManager.fullscreenScaling) {
                        ForEach(FullscreenScaling.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    if configManager.renderer.wrappedValue == .vulkan {
                        Toggle("Accurate Barriers", isOn: configManager.vkAccurateBarriers)
                    }
                    
                    if configManager.renderer.wrappedValue == .metal {
                        Toggle("Force Mesh Shaders", isOn: configManager.forceMeshShaders)
                        Toggle("Framebuffer Fetch", isOn: configManager.framebufferFetch)
                    }
                    
                    Toggle("Override App Gamma", isOn: configManager.overrideAppGammaPreference)
                    
                    if configManager.overrideAppGammaPreference.wrappedValue {
                        VStack(alignment: .leading) {
                            HStack {
                                Text("Override Gamma")
                                Spacer()
                                Text(String(format: "%.2f", configManager.overrideGammaValue.wrappedValue))
                                    .foregroundStyle(.secondary)
                            }
                            Slider(value: configManager.overrideGammaValue, in: 1.0...3.0, step: 0.05)
                        }
                    }
                    
                    VStack(alignment: .leading) {
                        HStack {
                            Text("Display Gamma")
                            Spacer()
                            Text(String(format: "%.2f", configManager.userDisplayGamma.wrappedValue))
                                .foregroundStyle(.secondary)
                        }
                        Slider(value: configManager.userDisplayGamma, in: 1.0...3.0, step: 0.05)
                    }
                    
                    
                    NavigationLink("All Graphic Packs") {
                        GraphicPacksView()
                    }
                }

                Section {
                    Text("These options may improve performance or thermals but are experimental and can cause graphical glitches, timing problems, crashes, or game-compatibility issues. All are off by default. Turn them on one at a time so you can tell which actually helps on your device.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)

                    VStack(alignment: .leading, spacing: 4) {
                        Toggle("Aggressive GPU Wait Backoff", isOn: configManager.experimentalAggressiveGpuWait)
                        Text("When the CPU is stalled waiting on the GPU (fence and semaphore waits), let the waiting thread sleep sooner instead of busy-spinning a core. May reduce heat while GPU-bound; can add a little CPU–GPU latency in heavily GPU-bound scenes.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Toggle("Aggressive Frame-Pacing Backoff", isOn: configManager.experimentalAggressiveFramePacing)
                        Text("Spend less time busy-waiting for the next display flip and for command-buffer space, sleeping through most of the idle interval instead. This is the biggest idle-core saver but also the most timing-sensitive: too aggressive can cause micro-stutter.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    if configManager.renderer.wrappedValue == .metal {
                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Skip Redundant GPU Residency", isOn: configManager.experimentalSkipRedundantResidency)
                            Text("Metal only. Avoid re-declaring the same textures and buffers as resident on every draw within a render pass. Cuts per-draw driver work; if it misbehaves it can cause rendering artifacts.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Toggle("PPC Block Linking", isOn: configManager.experimentalPpcBlockLinking)
                        Text("Let the CPU interpreter jump straight from one block of guest code to the next instead of looking each one up in a hash table every time. Can reduce CPU load and heat; if it misbehaves it may cause instability or crashes. Self-clears whenever games modify their own code.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    if configManager.renderer.wrappedValue == .metal {
                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Pipeline State Fast-Path", isOn: configManager.experimentalPipelineCacheFastPath)
                            Text("Metal only. Reuse the last render-pipeline and depth-stencil state within a single draw pass instead of re-hashing and re-looking-them-up on every draw. Cuts per-draw CPU work; if it misbehaves it can cause rendering glitches or crashes.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Sampler State Fast-Path", isOn: configManager.experimentalSamplerCacheFastPath)
                            Text("Metal only. Reuse texture sampler state within a single draw pass instead of re-hashing it for every texture on every draw — this is the most frequent lookup in the renderer. Cuts per-draw CPU work; if it misbehaves it can cause texture-filtering glitches or crashes.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Extended Command Batching", isOn: configManager.experimentalExtendedCommitThreshold)
                            Text("Metal only. Submit larger batches of GPU work per command buffer, reducing how often work is flushed to the GPU. Rendering is unchanged, but larger batches raise GPU latency and memory use, which can shift frame pacing or, in extreme scenes, trigger a GPU timeout.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Skip Redundant Surface-Copy Loads", isOn: configManager.experimentalSurfaceCopyDestDontCare)
                            Text("Metal only. When a surface copy completely overwrites its destination, skip loading the destination's old pixels first. Saves memory bandwidth and heat on the GPU. Partial copies are unaffected. If it misbehaves it can cause momentary garbage in copied surfaces.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Skip Redundant Present Load", isOn: configManager.experimentalPresentDontCare)
                            Text("Metal only. The final image is drawn edge-to-edge over the whole screen every frame, so skip loading the previous frame's pixels into the display's tile memory first. Saves memory bandwidth and heat on the GPU. Letterboxed output still clears its borders. If it misbehaves it can cause momentary garbage at the screen edges.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Submit GPU Work Earlier", isOn: configManager.experimentalCommitOnCpIdle)
                            Text("Metal only. When the game briefly runs out of drawing commands to send, hand the queued work to the GPU right away instead of waiting for more to pile up. Lets the GPU start sooner and overlap with the CPU, which can raise the frame rate when the game is CPU-bound. Does not make the CPU work any harder. Rarely, submitting more often can add a little overhead.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                } header: {
                    Text("Experimental Performance")
                } footer: {
                    Text("With all of these off, the emulator behaves the same as it does normally.")
                }

                if configManager.renderer.wrappedValue == .metal {
                    Section {
                        Text("Experimental. MetalFX renders the game at a lower internal resolution and upscales the final image with Apple's spatial scaler, which can lower GPU load and heat. It can also introduce upscaling artifacts, softer or shimmering image quality, and frame-pacing changes, and may behave differently per game. Requires iOS 16 or newer on a MetalFX-capable device; on unsupported devices it stays off automatically. All options are off by default and apply after relaunching the game.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Enable MetalFX Upscaling", isOn: configManager.experimentalMetalFXEnable)
                            Text("Master switch. When off, the renderer behaves exactly as normal — native resolution, no scaler, no extra GPU resources. When on, the game renders at the resolution below and MetalFX upscales it to your display.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Picker("MetalFX Mode", selection: configManager.experimentalMetalFXMode) {
                                Text("Spatial").tag(0)
                                Text("Temporal (Unavailable)").tag(1).disabled(true)
                            }
                            Text("Spatial upscaling works from the finished frame alone. Temporal upscaling is not available: it requires per-frame depth, motion vectors, and a subpixel jitter sequence that Wii U titles do not produce and that cannot be synthesized without fabricating data, so enabling it would only add ghosting.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Text("Internal Resolution: \(configManager.experimentalMetalFXRenderScale.wrappedValue)%")
                            Slider(
                                value: Binding(
                                    get: { Double(configManager.experimentalMetalFXRenderScale.wrappedValue) },
                                    set: { configManager.experimentalMetalFXRenderScale.wrappedValue = Int($0.rounded()) }
                                ),
                                in: 25...100,
                                step: 1
                            )
                            Text("How much to lower the render resolution before upscaling. Drag for any value from 25% to 100%. Lower percentages do less GPU work but look softer; 100% renders at native resolution and skips upscaling entirely. Only used while MetalFX is enabled.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Selective Render Scaling", isOn: configManager.experimentalMetalFXSelectiveScaling)
                            Text("Only lower the resolution of large render targets (the main scene and full-screen effects) and keep small targets — HUD, menus, and intermediate buffers — at native resolution. This can keep 2D and text sharper and reduce overhead, with a smaller GPU saving than scaling everything. Only used while MetalFX is enabled.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Direct Input (skip upscaler copy)", isOn: configManager.experimentalMetalFXDirectInput)
                            Text("Experimental. Feed the reduced-resolution frame straight into MetalFX instead of copying it into the upscaler's own buffer first, saving a per-frame copy. Frames that are not compatible fall back to the copy automatically. Only used while MetalFX is enabled. Leave off if you notice any flicker or visual glitches while upscaling.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        VStack(alignment: .leading, spacing: 4) {
                            Toggle("Sharp Present (skip redundant filter)", isOn: configManager.experimentalMetalFXSharpPresent)
                            Text("Experimental. After MetalFX has already upscaled the frame to display size, skip the extra upscaling filter (bicubic/Hermite) on the final copy to the screen, which would only re-sample the image and slightly soften it. Only has an effect while MetalFX is upscaling. The picture may look slightly sharper; turn off if you prefer the softer look.")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    } header: {
                        Text("Experimental Graphics")
                    } footer: {
                        Text("MetalFX is Metal-only. With it off, rendering is identical to normal. If your device does not support MetalFX, the emulator falls back to normal rendering without crashing.")
                    }
                }

                Section("Audio") {
                    Toggle("TV Audio", isOn: configManager.tvAudioEnabled)
                    Toggle("Pad Audio", isOn: configManager.padAudioEnabled)
                    
                    Picker("TV Channels", selection: configManager.tvChannels) {
                        ForEach(AudioChannels.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Picker("Pad Channels", selection: configManager.padChannels) {
                        ForEach(AudioChannels.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Picker("Input Channels", selection: configManager.inputChannels) {
                        ForEach(AudioChannels.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    Toggle("Microphone", isOn: configManager.microphoneEnabled)

                    VStack(alignment: .leading, spacing: 4) {
                        Picker("Audio Buffering (Experimental)", selection: configManager.experimentalAudioBufferBlocks) {
                            Text("Default").tag(0)
                            Text("~48 ms").tag(4)
                            Text("~72 ms").tag(6)
                            Text("~96 ms").tag(8)
                            Text("~120 ms").tag(10)
                        }
                        Text("Experimental. Buffers more audio ahead of playback to prevent crackling, popping, or dropouts caused by CPU load or timing hiccups. Higher settings add latency (audio lags slightly behind the picture). \"Default\" restores normal behavior. Applies on the next game launch.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Picker("Anti-Clip / Limiter (Experimental)", selection: configManager.experimentalAudioAntiClip) {
                            Text("Off").tag(0)
                            Text("Soft Limiter").tag(1)
                            Text("Headroom -3 dB").tag(2)
                            Text("Headroom -6 dB").tag(3)
                        }
                        Text("Experimental. Reduces harsh distortion on loud audio (e.g. dialogue over music) that the console's mixer clips at full scale. \"Soft Limiter\" leaves quiet audio untouched and only rounds peaks that would clip (preserves loudness). \"Headroom\" lowers the whole mix a fixed amount so peaks stay below clipping (also lowers overall volume). \"Off\" restores normal behavior.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("TV Volume: \(Int(configManager.tvVolume.wrappedValue))%")
                        Slider(value: configManager.tvVolume, in: 0...100, step: 1)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Pad Volume: \(Int(configManager.padVolume.wrappedValue))%")
                        Slider(value: configManager.padVolume, in: 0...100, step: 1)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Input Volume: \(Int(configManager.inputVolume.wrappedValue))%")
                        Slider(value: configManager.inputVolume, in: 0...100, step: 1)
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Portal Volume: \(Int(configManager.portalVolume.wrappedValue))%")
                        Slider(value: configManager.portalVolume, in: 0...100, step: 1)
                    }
                }
                
                Section("Overlay") {
                    Picker("Position", selection: configManager.overlayPosition) {
                        ForEach(ScreenPosition.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    TextField("Text Color", text: configManager.overlayTextColorHex)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    
                    VStack(alignment: .leading) {
                        Text("Text Scale: \(Int(configManager.overlayTextScale.wrappedValue))%")
                        Slider(value: configManager.overlayTextScale, in: 50...200, step: 25)
                    }
                    
                    Toggle("FPS", isOn: configManager.overlayFPS)
                    Toggle("CPU Mode", isOn: configManager.overlayCPUMode)
                    Toggle("Draw Calls", isOn: configManager.overlayDrawcalls)
                    Toggle("CPU Usage", isOn: configManager.overlayCPUUsage)
                    Toggle("CPU Per Core Usage", isOn: configManager.overlayCPUPerCoreUsage)
                    Toggle("RAM Usage", isOn: configManager.overlayRAMUsage)
                    Toggle("VRAM Usage", isOn: configManager.overlayVRAMUsage)
                    Toggle("Debug", isOn: configManager.overlayDebug)
                }
                
                Section("Notifications") {
                    Picker("Position", selection: configManager.notificationPosition) {
                        ForEach(ScreenPosition.allCases, id: \.self) {
                            Text($0.string)
                        }
                    }
                    
                    TextField("Text Color", text: configManager.notificationTextColorHex)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    
                    VStack(alignment: .leading) {
                        Text("Text Scale: \(Int(configManager.notificationTextScale.wrappedValue))%")
                        Slider(value: configManager.notificationTextScale, in: 50...200, step: 25)
                    }
                    
                    Toggle("Controller Profiles", isOn: configManager.notificationControllerProfiles)
                    Toggle("Low Battery", isOn: configManager.notificationControllerBattery)
                    Toggle("Shader Compiling", isOn: configManager.notificationShaderCompiling)
                    Toggle("Friends", isOn: configManager.notificationFriends)
                }
                
                Section("Input Services") {
                    Toggle("Disable Motion", isOn: configManager.disableMotion)
                    
                    //TextField("DSU Host", text: configManager.dsuHost)
                    //    .textInputAutocapitalization(.never)
                    //    .autocorrectionDisabled()
                    
                    //TextField("DSU Port", text: configManager.dsuPortString)
                    //    .keyboardType(.numberPad)
                }
                
                Section("Emulated Devices") {
                    Toggle("Skylanders Portal", isOn: configManager.emulateSkylanderPortal)
                    Toggle("Disney Infinity Base", isOn: configManager.emulateInfinityBase)
                    Toggle("LEGO Dimensions Toypad", isOn: configManager.emulateDimensionsToypad)
                    Button {
                        showingEmulatedDevices = true
                    } label: {
                        Label("Manage Figures", systemImage: "externaldrive.connected.to.line.below")
                    }
                    .sheet(isPresented: $showingEmulatedDevices) {
                        EmulatedDevicesView()
                    }
                }
                
                Section("Load Game") {
                    Button("Load from Folder") {
                        FileImporterManager.shared.importFiles(
                            types: [.folder],
                            allowMultiple: false,
                            stopAccessingSecurityScopedResources: false
                        ) { result in
                            handleImportResult(result)
                        }
                    }
                    
                    Button("Load from File") {
                        FileImporterManager.shared.importFiles(
                            types: [.item],
                            allowMultiple: false,
                            stopAccessingSecurityScopedResources: false
                        ) { result in
                            handleImportResult(result)
                        }
                    }
                }
                
                Section {
                    HStack {
                        Text("In memoriam of 'Lily'")
                            .font(.system(.footnote, design: .monospaced))
                            .foregroundColor(.secondary)
                        Image(systemName: "heart")
                            .foregroundColor(.purple)
                            .font(.footnote)
                    }
                    .frame(maxWidth: .infinity, alignment: .center)
                } 
            }
            .navigationTitle("Settings") // iOS 15 seems to expect a navigation title, so we'll put this here. -stossy11
        }
    }
    
    private func handleImportResult(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            _ = url.startAccessingSecurityScopedResource()
            gameManager.loadGame(url.path, true)
        case .failure(let error):
            print("Failed to import: \(error)")
        }
    }
}

private struct ControllerRow: View {
    let entry: ControllerEntry

    var body: some View {
        HStack {
            Image(systemName: entry.isVirtual ? "iphone" : "gamecontroller.fill")
                .foregroundStyle(entry.isVirtual ? .blue : .primary)
                .frame(width: 28)

            VStack(alignment: .leading, spacing: 2) {
                Text(entry.name)
                    .font(.body)
                Text(entry.controllerType.name)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }
}
