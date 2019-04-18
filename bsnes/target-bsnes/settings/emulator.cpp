auto EmulatorSettings::create() -> void {
  setIcon(Icon::Action::Settings);
  setText("Emulator");

  layout.setPadding(5_sx);

  optionsLabel.setText("Options").setFont(Font().setBold());
  inputFocusLabel.setText("When focus is lost:");
  pauseEmulation.setText("Pause emulation").onActivate([&] {
    settings.input.defocus = "Pause";
  });
  blockInput.setText("Block input").onActivate([&] {
    settings.input.defocus = "Block";
  });
  allowInput.setText("Allow input").onActivate([&] {
    settings.input.defocus = "Allow";
  });
  if(settings.input.defocus == "Pause") pauseEmulation.setChecked();
  if(settings.input.defocus == "Block") blockInput.setChecked();
  if(settings.input.defocus == "Allow") allowInput.setChecked();
  warnOnUnverifiedGames.setText("Warn when loading games that have not been verified").setChecked(settings.emulator.warnOnUnverifiedGames).onToggle([&] {
    settings.emulator.warnOnUnverifiedGames = warnOnUnverifiedGames.checked();
  });
  autoSaveMemory.setText("Auto-save memory periodically").setChecked(settings.emulator.autoSaveMemory.enable).onToggle([&] {
    settings.emulator.autoSaveMemory.enable = autoSaveMemory.checked();
  });
  autoSaveStateOnUnload.setText("Auto-save undo state when unloading games").setChecked(settings.emulator.autoSaveStateOnUnload).onToggle([&] {
    settings.emulator.autoSaveStateOnUnload = autoSaveStateOnUnload.checked();
    if(!autoSaveStateOnUnload.checked()) {
      autoLoadStateOnLoad.setEnabled(false).setChecked(false).doToggle();
    } else {
      autoLoadStateOnLoad.setEnabled(true);
    }
  }).doToggle();
  autoLoadStateOnLoad.setText("Auto-resume on load").setChecked(settings.emulator.autoLoadStateOnLoad).onToggle([&] {
    settings.emulator.autoLoadStateOnLoad = autoLoadStateOnLoad.checked();
  });
  optionsSpacer.setColor({192, 192, 192});

  ppuLabel.setText("PPU (video)").setFont(Font().setBold());
  fastPPU.setText("Fast mode").setChecked(settings.emulator.hack.ppu.fast).onToggle([&] {
    settings.emulator.hack.ppu.fast = fastPPU.checked();
    if(!fastPPU.checked()) {
      noSpriteLimit.setEnabled(false).setChecked(false).doToggle();
      hiresMode7.setEnabled(false).setChecked(false).doToggle();
    } else {
      noSpriteLimit.setEnabled(true);
      hiresMode7.setEnabled(true);
    }
  }).doToggle();
  noSpriteLimit.setText("No sprite limit").setChecked(settings.emulator.hack.ppu.noSpriteLimit).onToggle([&] {
    settings.emulator.hack.ppu.noSpriteLimit = noSpriteLimit.checked();
  });
  hiresMode7.setText("Hires mode 7").setChecked(settings.emulator.hack.ppu.mode7.hires).setVisible(false).onToggle([&] {
    settings.emulator.hack.ppu.mode7.hires = hiresMode7.checked();
    emulator->configure("Hacks/PPU/Mode7/Hires", settings.emulator.hack.ppu.mode7.hires);
  });
  mode7Label.setText("HD Mode 7 (fast PPU only)").setFont(Font().setBold());
  mode7ScaleLabel.setText("Scale:");
  mode7Scale.append(ComboButtonItem().setText( "240p").setProperty("multiplier", 1));
  mode7Scale.append(ComboButtonItem().setText( "480p").setProperty("multiplier", 2));
  mode7Scale.append(ComboButtonItem().setText( "720p").setProperty("multiplier", 3));
  mode7Scale.append(ComboButtonItem().setText( "960p").setProperty("multiplier", 4));
  mode7Scale.append(ComboButtonItem().setText("1200p").setProperty("multiplier", 5));
  mode7Scale.append(ComboButtonItem().setText("1440p").setProperty("multiplier", 6));
  mode7Scale.append(ComboButtonItem().setText("1680p").setProperty("multiplier", 7));
  mode7Scale.append(ComboButtonItem().setText("1920p").setProperty("multiplier", 8));
  mode7Scale.append(ComboButtonItem().setText("2160p").setProperty("multiplier", 9));
  for(uint n = 1; n <= 9; n++) {
    if(settings.emulator.hack.ppu.mode7.scale == n) mode7Scale.item(n - 1).setSelected();
  }
  mode7Scale.onChange([&] {
    settings.emulator.hack.ppu.mode7.scale = mode7Scale.selected().property("multiplier").natural();
    emulator->configure("Hacks/PPU/Mode7/Scale", settings.emulator.hack.ppu.mode7.scale);
  });
  mode7Perspective.setText("Perspective correction").setChecked(settings.emulator.hack.ppu.mode7.perspective).onToggle([&] {
    settings.emulator.hack.ppu.mode7.perspective = mode7Perspective.checked();
    emulator->configure("Hacks/PPU/Mode7/Perspective", settings.emulator.hack.ppu.mode7.perspective);
  });
  dspLabel.setText("DSP (audio)").setFont(Font().setBold());
  fastDSP.setText("Fast mode").setChecked(settings.emulator.hack.dsp.fast).onToggle([&] {
    settings.emulator.hack.dsp.fast = fastDSP.checked();
    emulator->configure("Hacks/DSP/Fast", settings.emulator.hack.dsp.fast);
  });
  cubicInterpolation.setText("Cubic interpolation").setChecked(settings.emulator.hack.dsp.cubic).onToggle([&] {
    settings.emulator.hack.dsp.cubic = cubicInterpolation.checked();
    emulator->configure("Hacks/DSP/Cubic", settings.emulator.hack.dsp.cubic);
  });
  coprocessorLabel.setText("Coprocessors").setFont(Font().setBold());
  coprocessorsDelayedSyncOption.setText("Fast mode").setChecked(settings.emulator.hack.coprocessors.delayedSync).onToggle([&] {
    settings.emulator.hack.coprocessors.delayedSync = coprocessorsDelayedSyncOption.checked();
  });
  coprocessorsHLEOption.setText("Prefer HLE").setChecked(settings.emulator.hack.coprocessors.hle).onToggle([&] {
    settings.emulator.hack.coprocessors.hle = coprocessorsHLEOption.checked();
  });
  superFXLabel.setText("SuperFX clock speed:");
  superFXValue.setAlignment(0.5);
  superFXClock.setLength(71).setPosition((settings.emulator.hack.fastSuperFX - 100) / 10).onChange([&] {
    settings.emulator.hack.fastSuperFX = superFXClock.position() * 10 + 100;
    superFXValue.setText({settings.emulator.hack.fastSuperFX, "%"});
  }).doChange();
  hacksNote.setForegroundColor({224, 0, 0}).setText("Note: some hack setting changes do not take effect until after reloading games.");
}

auto EmulatorSettings::updateConfiguration() -> void {
  emulator->configure("Hacks/PPU/Fast", fastPPU.checked());
  emulator->configure("Hacks/PPU/NoSpriteLimit", noSpriteLimit.checked());
  emulator->configure("Hacks/PPU/Mode7/Hires", hiresMode7.checked());
  emulator->configure("Hacks/PPU/Mode7/Scale", mode7Scale.selected().property("multiplier").natural());
  emulator->configure("Hacks/DSP/Fast", fastDSP.checked());
  emulator->configure("Hacks/DSP/Cubic", cubicInterpolation.checked());
  emulator->configure("Hacks/Coprocessor/DelayedSync", coprocessorsDelayedSyncOption.checked());
  emulator->configure("Hacks/Coprocessor/HLE", coprocessorsHLEOption.checked());
}
