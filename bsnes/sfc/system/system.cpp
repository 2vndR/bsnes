#include <sfc/sfc.hpp>

namespace SuperFamicom {

System system;
Scheduler scheduler;
Random random;
Cheat cheat;
#include "serialization.cpp"

auto System::run() -> void {
  scheduler.mode = Scheduler::Mode::Run;
  scheduler.enter();
  if(scheduler.event == Scheduler::Event::Frame) frameEvent();
}

auto System::runToSave() -> void {
  //run the CPU thread normally, exiting once we reach the next synchronization point.
  //the CPU thread may run in the pathological case for up to 10 frames for a long 8-channel x 64KB DMA transfer.
  //in practice, this will typically be at most one DMA transfer.
  scheduler.mode = Scheduler::Mode::SynchronizeCPU;
  runToSynchronize();

  //now synchronize every other thread to their synchronization points.
  //stop after each thread is very slightly ahead of the CPU thread.
  scheduler.mode = Scheduler::Mode::SynchronizeAll;
  runToThread(smp);
  runToThread(ppu);
  for(auto coprocessor : cpu.coprocessors) runToThread(*coprocessor);

  //at this point, the CPU thread is the furthest behind in time.
  //all threads are now at their synchronization points and can be serialized safely.
  scheduler.mode = Scheduler::Mode::Run;
  scheduler.active = cpu.thread;
}

auto System::runToSynchronize() -> void {
  while(true) {
    scheduler.enter();
    if(scheduler.event == Scheduler::Event::Frame) frameEvent();
    if(scheduler.event == Scheduler::Event::Synchronize) break;
  }
}

auto System::runToThread(Thread& aux) -> void {
  //first, ensure that the CPU is ahead of the thread we want to synchronize to.
  //because if it isn't, and the other thread is ahead, it will run even more ahead to synchronize itself.
  scheduler.active = cpu.thread;
  while(aux.clock >= 0) runToSynchronize();

  //now that it is, run the other thread until it's just barely surpassed the CPU thread.
  scheduler.active = aux.thread;
  do runToSynchronize(); while(aux.clock < 0);
}

auto System::frameEvent() -> void {
  ppu.refresh();

  //refresh all cheat codes once per frame
  Memory::GlobalWriteEnable = true;
  for(auto& code : cheat.codes) {
    if(code.enable) {
      bus.write(code.address, code.data);
    }
  }
  Memory::GlobalWriteEnable = false;
}

auto System::load(Emulator::Interface* interface) -> bool {
  information = {};

  bus.reset();
  if(!cpu.load()) return false;
  if(!smp.load()) return false;
  if(!ppu.load()) return false;
  if(!dsp.load()) return false;
  if(!cartridge.load()) return false;

  if(cartridge.region() == "NTSC") {
    information.region = Region::NTSC;
    information.cpuFrequency = Emulator::Constants::Colorburst::NTSC * 6.0;
  }
  if(cartridge.region() == "PAL") {
    information.region = Region::PAL;
    information.cpuFrequency = Emulator::Constants::Colorburst::PAL * 4.8;
  }

  if(cartridge.has.ICD) {
    if(!icd.load()) return false;
  }
  if(cartridge.has.BSMemorySlot) bsmemory.load();

  serializeInit();
  this->interface = interface;
  return information.loaded = true;
}

auto System::save() -> void {
  if(!loaded()) return;

  cartridge.save();
}

auto System::unload() -> void {
  if(!loaded()) return;

  controllerPort1.unload();
  controllerPort2.unload();
  expansionPort.unload();

  if(cartridge.has.ICD) icd.unload();
  if(cartridge.has.MCC) mcc.unload();
  if(cartridge.has.Event) event.unload();
  if(cartridge.has.SA1) sa1.unload();
  if(cartridge.has.SuperFX) superfx.unload();
  if(cartridge.has.HitachiDSP) hitachidsp.unload();
  if(cartridge.has.SPC7110) spc7110.unload();
  if(cartridge.has.SDD1) sdd1.unload();
  if(cartridge.has.OBC1) obc1.unload();
  if(cartridge.has.MSU1) msu1.unload();
  if(cartridge.has.BSMemorySlot) bsmemory.unload();
  if(cartridge.has.SufamiTurboSlotA) sufamiturboA.unload();
  if(cartridge.has.SufamiTurboSlotB) sufamiturboB.unload();

  cartridge.unload();
  information.loaded = false;
}

auto System::power(bool reset) -> void {
  hacks.fastPPU = configuration.hacks.ppu.fast;

  Emulator::audio.reset(interface);

  random.entropy(Random::Entropy::Low);  //fallback case
  if(configuration.hacks.entropy == "None") random.entropy(Random::Entropy::None);
  if(configuration.hacks.entropy == "Low" ) random.entropy(Random::Entropy::Low );
  if(configuration.hacks.entropy == "High") random.entropy(Random::Entropy::High);

  cpu.power(reset);
  smp.power(reset);
  dsp.power(reset);
  ppu.power(reset);

  if(cartridge.has.ICD) icd.power();
  if(cartridge.has.MCC) mcc.power();
  if(cartridge.has.DIP) dip.power();
  if(cartridge.has.Event) event.power();
  if(cartridge.has.SA1) sa1.power();
  if(cartridge.has.SuperFX) superfx.power();
  if(cartridge.has.ARMDSP) armdsp.power();
  if(cartridge.has.HitachiDSP) hitachidsp.power();
  if(cartridge.has.NECDSP) necdsp.power();
  if(cartridge.has.EpsonRTC) epsonrtc.power();
  if(cartridge.has.SharpRTC) sharprtc.power();
  if(cartridge.has.SPC7110) spc7110.power();
  if(cartridge.has.SDD1) sdd1.power();
  if(cartridge.has.OBC1) obc1.power();
  if(cartridge.has.MSU1) msu1.power();
  if(cartridge.has.Cx4) cx4.power();
  if(cartridge.has.DSP1) dsp1.power();
  if(cartridge.has.DSP2) dsp2.power();
  if(cartridge.has.DSP4) dsp4.power();
  if(cartridge.has.ST0010) st0010.power();
  if(cartridge.has.BSMemorySlot) bsmemory.power();
  if(cartridge.has.SufamiTurboSlotA) sufamiturboA.power();
  if(cartridge.has.SufamiTurboSlotB) sufamiturboB.power();

  if(cartridge.has.ICD) cpu.coprocessors.append(&icd);
  if(cartridge.has.Event) cpu.coprocessors.append(&event);
  if(cartridge.has.SA1) cpu.coprocessors.append(&sa1);
  if(cartridge.has.SuperFX) cpu.coprocessors.append(&superfx);
  if(cartridge.has.ARMDSP) cpu.coprocessors.append(&armdsp);
  if(cartridge.has.HitachiDSP) cpu.coprocessors.append(&hitachidsp);
  if(cartridge.has.NECDSP) cpu.coprocessors.append(&necdsp);
  if(cartridge.has.EpsonRTC) cpu.coprocessors.append(&epsonrtc);
  if(cartridge.has.SharpRTC) cpu.coprocessors.append(&sharprtc);
  if(cartridge.has.SPC7110) cpu.coprocessors.append(&spc7110);
  if(cartridge.has.MSU1) cpu.coprocessors.append(&msu1);
  if(cartridge.has.BSMemorySlot) cpu.coprocessors.append(&bsmemory);

  scheduler.active = cpu.thread;

  controllerPort1.power(ID::Port::Controller1);
  controllerPort2.power(ID::Port::Controller2);
  expansionPort.power();

  controllerPort1.connect(settings.controllerPort1);
  controllerPort2.connect(settings.controllerPort2);
  expansionPort.connect(settings.expansionPort);
}

}
