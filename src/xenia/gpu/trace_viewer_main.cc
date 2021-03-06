/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <gflags/gflags.h>

#include "poly/main.h"
#include "poly/mapped_memory.h"
#include "poly/math.h"
#include "third_party/imgui/imgui.h"
#include "xenia/gpu/gl4/gl_context.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/tracing.h"
#include "xenia/gpu/xenos.h"
#include "xenia/emulator.h"
#include "xenia/ui/main_window.h"

// HACK: until we have another impl, we just use gl4 directly.
#include "xenia/gpu/gl4/command_processor.h"
#include "xenia/gpu/gl4/gl4_graphics_system.h"
#include "xenia/gpu/gl4/gl4_shader.h"

DEFINE_string(target_trace_file, "", "Specifies the trace file to load.");

namespace xe {
namespace gpu {

enum class PacketCategory {
  kGeneric,
  kDraw,
  kSwap,
};
struct PacketTypeInfo {
  PacketCategory category;
  const char* name;
};
struct PacketAction {
  enum class Type {
    kRegisterWrite,
  };
  Type type;
  union {
    struct {
      uint32_t index;
      RegisterFile::RegisterValue value;
    } register_write;
  };
  static PacketAction RegisterWrite(uint32_t index, uint32_t value) {
    PacketAction action;
    action.type = Type::kRegisterWrite;
    action.register_write.index = index;
    action.register_write.value.u32 = value;
    return action;
  }
};
struct PacketInfo {
  const PacketTypeInfo* type_info;
  bool predicated;
  uint32_t count;
  std::vector<PacketAction> actions;
};
bool DisasmPacketType0(const uint8_t* base_ptr, uint32_t packet,
                       PacketInfo* out_info) {
  static const PacketTypeInfo type_0_info = {PacketCategory::kGeneric,
                                             "PM4_TYPE0"};
  out_info->type_info = &type_0_info;

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  out_info->count = 1 + count;
  auto ptr = base_ptr + 4;

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = poly::load_and_swap<uint32_t>(ptr);
    uint32_t target_index = write_one_reg ? base_index : base_index + m;
    out_info->actions.emplace_back(
        PacketAction::RegisterWrite(target_index, reg_data));
    ptr += 4;
  }

  return true;
}
bool DisasmPacketType1(const uint8_t* base_ptr, uint32_t packet,
                       PacketInfo* out_info) {
  static const PacketTypeInfo type_1_info = {PacketCategory::kGeneric,
                                             "PM4_TYPE1"};
  out_info->type_info = &type_1_info;

  out_info->count = 1 + 2;
  auto ptr = base_ptr + 4;

  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = poly::load_and_swap<uint32_t>(ptr);
  uint32_t reg_data_2 = poly::load_and_swap<uint32_t>(ptr + 4);
  out_info->actions.emplace_back(
      PacketAction::RegisterWrite(reg_index_1, reg_data_1));
  out_info->actions.emplace_back(
      PacketAction::RegisterWrite(reg_index_2, reg_data_2));

  return true;
}
bool DisasmPacketType2(const uint8_t* base_ptr, uint32_t packet,
                       PacketInfo* out_info) {
  static const PacketTypeInfo type_2_info = {PacketCategory::kGeneric,
                                             "PM4_TYPE2"};
  out_info->type_info = &type_2_info;

  out_info->count = 1;

  return true;
}
using namespace xe::gpu::xenos;
bool DisasmPacketType3(const uint8_t* base_ptr, uint32_t packet,
                       PacketInfo* out_info) {
  static const PacketTypeInfo type_3_unknown_info = {PacketCategory::kGeneric,
                                                     "PM4_TYPE3_UNKNOWN"};
  out_info->type_info = &type_3_unknown_info;

  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  out_info->count = 1 + count;
  auto ptr = base_ptr + 4;

  if (packet & 1) {
    out_info->predicated = true;
  }

  bool result = true;
  switch (opcode) {
    case PM4_ME_INIT: {
      // initialize CP's micro-engine
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_ME_INIT"};
      out_info->type_info = &op_info;
      break;
    }
    case PM4_NOP: {
      // skip N 32-bit words to get to the next packet
      // No-op, ignore some data.
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_NOP"};
      out_info->type_info = &op_info;
      break;
    }
    case PM4_INTERRUPT: {
      // generate interrupt from the command stream
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_INTERRUPT"};
      out_info->type_info = &op_info;
      uint32_t cpu_mask = poly::load_and_swap<uint32_t>(ptr + 0);
      for (int n = 0; n < 6; n++) {
        if (cpu_mask & (1 << n)) {
          // graphics_system_->DispatchInterruptCallback(1, n);
        }
      }
      break;
    }
    case PM4_XE_SWAP: {
      // Xenia-specific VdSwap hook.
      // VdSwap will post this to tell us we need to swap the screen/fire an
      // interrupt.
      // 63 words here, but only the first has any data.
      static const PacketTypeInfo op_info = {PacketCategory::kSwap,
                                             "PM4_XE_SWAP"};
      out_info->type_info = &op_info;
      uint32_t frontbuffer_ptr = poly::load_and_swap<uint32_t>(ptr + 0);
      break;
    }
    case PM4_INDIRECT_BUFFER: {
      // indirect buffer dispatch
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_INDIRECT_BUFFER"};
      out_info->type_info = &op_info;
      uint32_t list_ptr = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t list_length = poly::load_and_swap<uint32_t>(ptr + 4);
      break;
    }
    case PM4_WAIT_REG_MEM: {
      // wait until a register or memory location is a specific value
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_WAIT_REG_MEM"};
      out_info->type_info = &op_info;
      uint32_t wait_info = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t poll_reg_addr = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t ref = poly::load_and_swap<uint32_t>(ptr + 8);
      uint32_t mask = poly::load_and_swap<uint32_t>(ptr + 12);
      uint32_t wait = poly::load_and_swap<uint32_t>(ptr + 16);
      break;
    }
    case PM4_REG_RMW: {
      // register read/modify/write
      // ? (used during shader upload and edram setup)
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_REG_RMW"};
      out_info->type_info = &op_info;
      uint32_t rmw_info = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t and_mask = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t or_mask = poly::load_and_swap<uint32_t>(ptr + 8);
      break;
    }
    case PM4_COND_WRITE: {
      // conditional write to memory or register
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_COND_WRITE"};
      out_info->type_info = &op_info;
      uint32_t wait_info = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t poll_reg_addr = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t ref = poly::load_and_swap<uint32_t>(ptr + 8);
      uint32_t mask = poly::load_and_swap<uint32_t>(ptr + 12);
      uint32_t write_reg_addr = poly::load_and_swap<uint32_t>(ptr + 16);
      uint32_t write_data = poly::load_and_swap<uint32_t>(ptr + 20);
      break;
    }
    case PM4_EVENT_WRITE: {
      // generate an event that creates a write to memory when completed
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_EVENT_WRITE"};
      out_info->type_info = &op_info;
      uint32_t initiator = poly::load_and_swap<uint32_t>(ptr + 0);
      break;
    }
    case PM4_EVENT_WRITE_SHD: {
      // generate a VS|PS_done event
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_EVENT_WRITE_SHD"};
      out_info->type_info = &op_info;
      uint32_t initiator = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t address = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t value = poly::load_and_swap<uint32_t>(ptr + 8);
      break;
    }
    case PM4_EVENT_WRITE_EXT: {
      // generate a screen extent event
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_EVENT_WRITE_EXT"};
      out_info->type_info = &op_info;
      uint32_t unk0 = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t unk1 = poly::load_and_swap<uint32_t>(ptr + 4);
      break;
    }
    case PM4_DRAW_INDX: {
      // initiate fetch of index buffer and draw
      // dword0 = viz query info
      static const PacketTypeInfo op_info = {PacketCategory::kDraw,
                                             "PM4_DRAW_INDX"};
      out_info->type_info = &op_info;
      uint32_t dword0 = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t dword1 = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t index_count = dword1 >> 16;
      auto prim_type = static_cast<PrimitiveType>(dword1 & 0x3F);
      uint32_t src_sel = (dword1 >> 6) & 0x3;
      if (src_sel == 0x0) {
        // Indexed draw.
        uint32_t guest_base = poly::load_and_swap<uint32_t>(ptr + 8);
        uint32_t index_size = poly::load_and_swap<uint32_t>(ptr + 12);
        auto endianness = static_cast<Endian>(index_size >> 30);
        index_size &= 0x00FFFFFF;
        bool index_32bit = (dword1 >> 11) & 0x1;
        index_size *= index_32bit ? 4 : 2;
      } else if (src_sel == 0x2) {
        // Auto draw.
      } else {
        // Unknown source select.
        assert_always();
      }
      break;
    }
    case PM4_DRAW_INDX_2: {
      // draw using supplied indices in packet
      static const PacketTypeInfo op_info = {PacketCategory::kDraw,
                                             "PM4_DRAW_INDX_2"};
      out_info->type_info = &op_info;
      uint32_t dword0 = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t index_count = dword0 >> 16;
      auto prim_type = static_cast<PrimitiveType>(dword0 & 0x3F);
      uint32_t src_sel = (dword0 >> 6) & 0x3;
      assert_true(src_sel == 0x2);  // 'SrcSel=AutoIndex'
      bool index_32bit = (dword0 >> 11) & 0x1;
      uint32_t indices_size = index_count * (index_32bit ? 4 : 2);
      auto index_ptr = ptr + 4;
      break;
    }
    case PM4_SET_CONSTANT: {
      // load constant into chip and to memory
      // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
      //                                     reg - 0x2000
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_SET_CONSTANT"};
      out_info->type_info = &op_info;
      uint32_t offset_type = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t index = offset_type & 0x7FF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      switch (type) {
        case 0:  // ALU
          index += 0x4000;
          break;
        case 1:  // FETCH
          index += 0x4800;
          break;
        case 2:  // BOOL
          index += 0x4900;
          break;
        case 3:  // LOOP
          index += 0x4908;
          break;
        case 4:  // REGISTERS
          index += 0x2000;
          break;
        default:
          assert_always();
          result = false;
          break;
      }
      for (uint32_t n = 0; n < count - 1; n++, index++) {
        uint32_t data = poly::load_and_swap<uint32_t>(ptr + 4 + n * 4);
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(index, data));
      }
      break;
    }
    case PM4_LOAD_ALU_CONSTANT: {
      // load constants from memory
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_LOAD_ALU_CONSTANT"};
      out_info->type_info = &op_info;
      uint32_t address = poly::load_and_swap<uint32_t>(ptr + 0);
      address &= 0x3FFFFFFF;
      uint32_t offset_type = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t index = offset_type & 0x7FF;
      uint32_t size_dwords = poly::load_and_swap<uint32_t>(ptr + 8);
      size_dwords &= 0xFFF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      switch (type) {
        case 0:  // ALU
          index += 0x4000;
          break;
        case 1:  // FETCH
          index += 0x4800;
          break;
        case 2:  // BOOL
          index += 0x4900;
          break;
        case 3:  // LOOP
          index += 0x4908;
          break;
        case 4:  // REGISTERS
          index += 0x2000;
          break;
        default:
          assert_always();
          return true;
      }
      for (uint32_t n = 0; n < size_dwords; n++, index++) {
        // Hrm, ?
        // poly::load_and_swap<uint32_t>(membase_ + GpuToCpu(address + n * 4));
        uint32_t data = 0xDEADBEEF;
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(index, data));
      }
      break;
    }
    case PM4_SET_SHADER_CONSTANTS: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_SET_SHADER_CONSTANTS"};
      out_info->type_info = &op_info;
      uint32_t offset_type = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t index = offset_type & 0xFFFF;
      for (uint32_t n = 0; n < count - 1; n++, index++) {
        uint32_t data = poly::load_and_swap<uint32_t>(ptr + 4 + n * 4);
        out_info->actions.emplace_back(
            PacketAction::RegisterWrite(index, data));
      }
      return true;
    }
    case PM4_IM_LOAD: {
      // load sequencer instruction memory (pointer-based)
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_IM_LOAD"};
      out_info->type_info = &op_info;
      uint32_t addr_type = poly::load_and_swap<uint32_t>(ptr + 0);
      auto shader_type = static_cast<ShaderType>(addr_type & 0x3);
      uint32_t addr = addr_type & ~0x3;
      uint32_t start_size = poly::load_and_swap<uint32_t>(ptr + 4);
      uint32_t start = start_size >> 16;
      uint32_t size_dwords = start_size & 0xFFFF;  // dwords
      assert_true(start == 0);
      break;
    }
    case PM4_IM_LOAD_IMMEDIATE: {
      // load sequencer instruction memory (code embedded in packet)
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_IM_LOAD_IMMEDIATE"};
      out_info->type_info = &op_info;
      uint32_t dword0 = poly::load_and_swap<uint32_t>(ptr + 0);
      uint32_t dword1 = poly::load_and_swap<uint32_t>(ptr + 4);
      auto shader_type = static_cast<ShaderType>(dword0);
      uint32_t start_size = dword1;
      uint32_t start = start_size >> 16;
      uint32_t size_dwords = start_size & 0xFFFF;  // dwords
      assert_true(start == 0);
      break;
    }
    case PM4_INVALIDATE_STATE: {
      // selective invalidation of state pointers
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_INVALIDATE_STATE"};
      out_info->type_info = &op_info;
      uint32_t mask = poly::load_and_swap<uint32_t>(ptr + 0);
      break;
    }
    case PM4_SET_BIN_MASK_LO: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_SET_BIN_MASK_LO"};
      out_info->type_info = &op_info;
      uint32_t value = poly::load_and_swap<uint32_t>(ptr);
      // bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      break;
    }
    case PM4_SET_BIN_MASK_HI: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_SET_BIN_MASK_HI"};
      out_info->type_info = &op_info;
      uint32_t value = poly::load_and_swap<uint32_t>(ptr);
      // bin_mask_ =
      //  (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      break;
    }
    case PM4_SET_BIN_SELECT_LO: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_SET_BIN_SELECT_LO"};
      out_info->type_info = &op_info;
      uint32_t value = poly::load_and_swap<uint32_t>(ptr);
      // bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      break;
    }
    case PM4_SET_BIN_SELECT_HI: {
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_SET_BIN_SELECT_HI"};
      out_info->type_info = &op_info;
      uint32_t value = poly::load_and_swap<uint32_t>(ptr);
      // bin_select_ =
      //  (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      break;
    }

    // Ignored packets - useful if breaking on the default handler below.
    case 0x50: {  // 0xC0015000 usually 2 words, 0xFFFFFFFF / 0x00000000
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_TYPE3_0x50"};
      out_info->type_info = &op_info;
      break;
    }
    case 0x51: {  // 0xC0015100 usually 2 words, 0xFFFFFFFF / 0xFFFFFFFF
      static const PacketTypeInfo op_info = {PacketCategory::kGeneric,
                                             "PM4_TYPE3_0x51"};
      out_info->type_info = &op_info;
      break;
    }
    default: {
      result = false;
      break;
    }
  }

  return result;
}
bool DisasmPacket(const uint8_t* base_ptr, PacketInfo* out_info) {
  std::memset(out_info, 0, sizeof(PacketInfo));

  const uint32_t packet = poly::load_and_swap<uint32_t>(base_ptr);
  const uint32_t packet_type = packet >> 30;
  switch (packet_type) {
    case 0x00:
      return DisasmPacketType0(base_ptr, packet, out_info);
    case 0x01:
      return DisasmPacketType1(base_ptr, packet, out_info);
    case 0x02:
      return DisasmPacketType2(base_ptr, packet, out_info);
    case 0x03:
      return DisasmPacketType3(base_ptr, packet, out_info);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

PacketCategory GetPacketCategory(const uint8_t* base_ptr) {
  const uint32_t packet = poly::load_and_swap<uint32_t>(base_ptr);
  const uint32_t packet_type = packet >> 30;
  switch (packet_type) {
    case 0x00:
    case 0x01:
    case 0x02: {
      return PacketCategory::kGeneric;
    }
    case 0x03: {
      uint32_t opcode = (packet >> 8) & 0x7F;
      switch (opcode) {
        case PM4_DRAW_INDX:
        case PM4_DRAW_INDX_2:
          return PacketCategory::kDraw;
        case PM4_XE_SWAP:
          return PacketCategory::kSwap;
        default:
          return PacketCategory::kGeneric;
      }
    }
    default: {
      assert_unhandled_case(packet_type);
      return PacketCategory::kGeneric;
    }
  }
}

// TODO(benvanik): move to tracing.h/cc

class TraceReader {
 public:
  struct Frame {
    struct Command {
      enum class Type {
        kDraw,
        kSwap,
      };
      const uint8_t* head_ptr;
      const uint8_t* start_ptr;
      const uint8_t* end_ptr;
      Type type;
      union {
        struct {
          //
        } draw;
        struct {
          //
        } swap;
      };
    };

    const uint8_t* start_ptr;
    const uint8_t* end_ptr;
    int command_count;
    std::vector<Command> commands;
  };

  TraceReader() : trace_data_(nullptr), trace_size_(0) {}
  ~TraceReader() = default;

  const Frame* frame(int n) const { return &frames_[n]; }
  int frame_count() const { return int(frames_.size()); }

  bool Open(const std::wstring& path) {
    Close();

    mmap_ = poly::MappedMemory::Open(path, poly::MappedMemory::Mode::kRead);
    if (!mmap_) {
      return false;
    }

    trace_data_ = reinterpret_cast<const uint8_t*>(mmap_->data());
    trace_size_ = mmap_->size();

    ParseTrace();

    return true;
  }

  void Close() {
    mmap_.reset();
    trace_data_ = nullptr;
    trace_size_ = 0;
  }

  // void Foo() {
  //  auto trace_ptr = trace_data;
  //  while (trace_ptr < trace_data + trace_size) {
  //    auto cmd_type = *reinterpret_cast<const TraceCommandType*>(trace_ptr);
  //    switch (cmd_type) {
  //      case TraceCommandType::kPrimaryBufferStart:
  //        break;
  //      case TraceCommandType::kPrimaryBufferEnd:
  //        break;
  //      case TraceCommandType::kIndirectBufferStart:
  //        break;
  //      case TraceCommandType::kIndirectBufferEnd:
  //        break;
  //      case TraceCommandType::kPacketStart:
  //        break;
  //      case TraceCommandType::kPacketEnd:
  //        break;
  //      case TraceCommandType::kMemoryRead:
  //        break;
  //      case TraceCommandType::kMemoryWrite:
  //        break;
  //      case TraceCommandType::kEvent:
  //        break;
  //    }
  //    /*trace_ptr = graphics_system->PlayTrace(
  //    trace_ptr, trace_size - (trace_ptr - trace_data),
  //    GraphicsSystem::TracePlaybackMode::kBreakOnSwap);*/
  //  }
  //}

 protected:
  void ParseTrace() {
    auto trace_ptr = trace_data_;
    Frame current_frame = {
        trace_ptr, nullptr, 0,
    };
    const PacketStartCommand* packet_start = nullptr;
    const uint8_t* packet_start_ptr = nullptr;
    const uint8_t* last_ptr = trace_ptr;
    bool pending_break = false;
    while (trace_ptr < trace_data_ + trace_size_) {
      ++current_frame.command_count;
      auto type =
          static_cast<TraceCommandType>(poly::load<uint32_t>(trace_ptr));
      switch (type) {
        case TraceCommandType::kPrimaryBufferStart: {
          auto cmd =
              reinterpret_cast<const PrimaryBufferStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case TraceCommandType::kPrimaryBufferEnd: {
          auto cmd =
              reinterpret_cast<const PrimaryBufferEndCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd);
          break;
        }
        case TraceCommandType::kIndirectBufferStart: {
          auto cmd =
              reinterpret_cast<const IndirectBufferStartCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case TraceCommandType::kIndirectBufferEnd: {
          auto cmd =
              reinterpret_cast<const IndirectBufferEndCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd);
          break;
        }
        case TraceCommandType::kPacketStart: {
          auto cmd = reinterpret_cast<const PacketStartCommand*>(trace_ptr);
          packet_start_ptr = trace_ptr;
          packet_start = cmd;
          trace_ptr += sizeof(*cmd) + cmd->count * 4;
          break;
        }
        case TraceCommandType::kPacketEnd: {
          auto cmd = reinterpret_cast<const PacketEndCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd);
          if (!packet_start_ptr) {
            continue;
          }
          auto packet_category =
              GetPacketCategory(packet_start_ptr + sizeof(*packet_start));
          switch (packet_category) {
            case PacketCategory::kDraw: {
              Frame::Command command;
              command.type = Frame::Command::Type::kDraw;
              command.head_ptr = packet_start_ptr;
              command.start_ptr = last_ptr;
              command.end_ptr = trace_ptr;
              current_frame.commands.push_back(std::move(command));
              last_ptr = trace_ptr;
              break;
            }
            case PacketCategory::kSwap: {
              //
              break;
            }
          }
          if (pending_break) {
            current_frame.end_ptr = trace_ptr;
            frames_.push_back(std::move(current_frame));
            current_frame.start_ptr = trace_ptr;
            current_frame.end_ptr = nullptr;
            current_frame.command_count = 0;
            pending_break = false;
          }
          break;
        }
        case TraceCommandType::kMemoryRead: {
          auto cmd = reinterpret_cast<const MemoryReadCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->length;
          break;
        }
        case TraceCommandType::kMemoryWrite: {
          auto cmd = reinterpret_cast<const MemoryWriteCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd) + cmd->length;
          break;
        }
        case TraceCommandType::kEvent: {
          auto cmd = reinterpret_cast<const EventCommand*>(trace_ptr);
          trace_ptr += sizeof(*cmd);
          switch (cmd->event_type) {
            case EventType::kSwap: {
              pending_break = true;
              break;
            }
          }
          break;
        }
        default:
          // Broken trace file?
          assert_unhandled_case(type);
          break;
      }
    }
    if (pending_break || current_frame.command_count) {
      current_frame.end_ptr = trace_ptr;
      frames_.push_back(std::move(current_frame));
    }
  }

  std::unique_ptr<poly::MappedMemory> mmap_;
  const uint8_t* trace_data_;
  size_t trace_size_;
  std::vector<Frame> frames_;
};

class TracePlayer : public TraceReader {
 public:
  TracePlayer(poly::ui::Loop* loop, GraphicsSystem* graphics_system)
      : loop_(loop),
        graphics_system_(graphics_system),
        current_frame_index_(0),
        current_command_index_(-1) {}
  ~TracePlayer() = default;

  GraphicsSystem* graphics_system() const { return graphics_system_; }
  int current_frame_index() const { return current_frame_index_; }

  const Frame* current_frame() const {
    if (current_frame_index_ > frame_count()) {
      return nullptr;
    }
    return frame(current_frame_index_);
  }

  void SeekFrame(int target_frame) {
    if (current_frame_index_ == target_frame) {
      return;
    }
    current_frame_index_ = target_frame;
    auto frame = current_frame();
    current_command_index_ = int(frame->commands.size()) - 1;

    assert_true(frame->start_ptr <= frame->end_ptr);
    graphics_system_->PlayTrace(
        frame->start_ptr, frame->end_ptr - frame->start_ptr,
        GraphicsSystem::TracePlaybackMode::kBreakOnSwap);
  }

  int current_command_index() const { return current_command_index_; }

  void SeekCommand(int target_command) {
    if (current_command_index_ == target_command) {
      return;
    }
    int previous_command_index = current_command_index_;
    current_command_index_ = target_command;
    if (current_command_index_ == -1) {
      return;
    }
    auto frame = current_frame();
    const auto& command = frame->commands[target_command];
    assert_true(frame->start_ptr <= command.end_ptr);
    if (target_command && previous_command_index == target_command - 1) {
      // Seek forward.
      const auto& previous_command = frame->commands[target_command - 1];
      graphics_system_->PlayTrace(
          previous_command.end_ptr, command.end_ptr - previous_command.end_ptr,
          GraphicsSystem::TracePlaybackMode::kBreakOnSwap);
    } else {
      // Full playback from frame start.
      graphics_system_->PlayTrace(
          frame->start_ptr, command.end_ptr - frame->start_ptr,
          GraphicsSystem::TracePlaybackMode::kBreakOnSwap);
    }
  }

 private:
  poly::ui::Loop* loop_;
  GraphicsSystem* graphics_system_;
  int current_frame_index_;
  int current_command_index_;
};

void DrawControllerUI(xe::ui::MainWindow* window, TracePlayer& player,
                      uint8_t* membase) {
  ImGui::SetNextWindowPos(ImVec2(5, 5), ImGuiSetCondition_FirstUseEver);
  if (!ImGui::Begin("Controller", nullptr, ImVec2(340, 60))) {
    ImGui::End();
    return;
  }

  int target_frame = player.current_frame_index();
  if (ImGui::Button("|<<")) {
    target_frame = 0;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reset to first frame");
  }
  ImGui::SameLine();
  if (ImGui::Button(">>", ImVec2(0, 0), true)) {
    if (target_frame + 1 < player.frame_count()) {
      ++target_frame;
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Next frame (hold for continuous)");
  }
  ImGui::SameLine();
  if (ImGui::Button(">>|")) {
    target_frame = player.frame_count() - 1;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Skip to last frame");
  }
  ImGui::SameLine();
  ImGui::SliderInt("", &target_frame, 0, player.frame_count() - 1);
  if (target_frame != player.current_frame_index()) {
    player.SeekFrame(target_frame);
  }
  ImGui::End();
}

void DrawCommandListUI(xe::ui::MainWindow* window, TracePlayer& player,
                       uint8_t* membase) {
  ImGui::SetNextWindowPos(ImVec2(5, 70), ImGuiSetCondition_FirstUseEver);
  if (!ImGui::Begin("Command List", nullptr, ImVec2(200, 640))) {
    ImGui::End();
    return;
  }

  static const TracePlayer::Frame* previous_frame = nullptr;
  auto frame = player.current_frame();
  if (!frame) {
    ImGui::End();
    return;
  }
  bool did_seek = false;
  if (previous_frame != frame) {
    did_seek = true;
    previous_frame = frame;
  }
  int command_count = int(frame->commands.size());
  int target_command = player.current_command_index();
  int column_width = int(ImGui::GetContentRegionMax().x);
  ImGui::Text("Frame #%d", player.current_frame_index());
  ImGui::Separator();
  if (ImGui::Button("reset")) {
    target_command = -1;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Reset to before any frame commands");
  }
  ImGui::SameLine();
  if (ImGui::Button("prev", ImVec2(0, 0), true)) {
    if (target_command >= 0) {
      --target_command;
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Move to the previous command (hold)");
  }
  ImGui::SameLine();
  if (ImGui::Button("next", ImVec2(0, 0), true)) {
    if (target_command < command_count - 1) {
      ++target_command;
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Move to the next command (hold)");
  }
  ImGui::SameLine();
  if (ImGui::Button("end")) {
    target_command = command_count - 1;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Move to the last command");
  }
  ImGui::PushItemWidth(float(column_width - 15));
  ImGui::SliderInt("", &target_command, -1, command_count - 1);
  ImGui::PopItemWidth();
  if (target_command != player.current_command_index()) {
    did_seek = true;
    player.SeekCommand(target_command);
  }
  ImGui::Separator();
  ImGui::BeginChild("command_list");
  ImGui::PushID(-1);
  bool is_selected = player.current_command_index() == -1;
  if (ImGui::Selectable("<start>", &is_selected)) {
    player.SeekCommand(-1);
  }
  ImGui::PopID();
  if (did_seek && target_command == -1) {
    ImGui::SetScrollPosHere();
  }

  for (int i = 0; i < int(frame->commands.size()); ++i) {
    ImGui::PushID(i);
    is_selected = i == player.current_command_index();
    const auto& command = frame->commands[i];
    const char* label;
    switch (command.type) {
      case TraceReader::Frame::Command::Type::kDraw:
        label = "Draw";
        break;
      case TraceReader::Frame::Command::Type::kSwap:
        label = "Swap";
        break;
    }
    if (ImGui::Selectable(label, &is_selected)) {
      player.SeekCommand(i);
    }
    ImGui::SameLine(column_width - 60);
    ImGui::Text("%d", i);
    ImGui::PopID();
    if (did_seek && target_command == i) {
      ImGui::SetScrollPosHere();
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

static const ImVec4 kColorError =
    ImVec4(255 / 255.0f, 0 / 255.0f, 0 / 255.0f, 255 / 255.0f);
static const ImVec4 kColorComment =
    ImVec4(42 / 255.0f, 179 / 255.0f, 0 / 255.0f, 255 / 255.0f);
static const ImVec4 kColorIgnored =
    ImVec4(100 / 255.0f, 100 / 255.0f, 100 / 255.0f, 255 / 255.0f);

void DrawMultilineString(const std::string& str) {
  size_t i = 0;
  bool done = false;
  while (!done && i < str.size()) {
    size_t next_i = str.find('\n', i);
    if (next_i == std::string::npos) {
      done = true;
      next_i = str.size() - 1;
    }
    auto line = str.substr(i, next_i - i);
    ImGui::Text("%s", line.c_str());
    i = next_i + 1;
  }
}

enum class ShaderDisplayType : int {
  kUcode,
  kTranslated,
  kHostDisasm,
};

ShaderDisplayType DrawShaderTypeUI() {
  static ShaderDisplayType shader_display_type = ShaderDisplayType::kUcode;
  ImGui::RadioButton("ucode", reinterpret_cast<int*>(&shader_display_type),
                     static_cast<int>(ShaderDisplayType::kUcode));
  ImGui::SameLine();
  ImGui::RadioButton("translated", reinterpret_cast<int*>(&shader_display_type),
                     static_cast<int>(ShaderDisplayType::kTranslated));
  ImGui::SameLine();
  ImGui::RadioButton("disasm", reinterpret_cast<int*>(&shader_display_type),
                     static_cast<int>(ShaderDisplayType::kHostDisasm));
  return shader_display_type;
}

void DrawShaderUI(xe::ui::MainWindow* window, TracePlayer& player,
                  uint8_t* membase, gl4::GL4Shader* shader,
                  ShaderDisplayType display_type) {
  // Must be prepared for advanced display modes.
  if (display_type != ShaderDisplayType::kUcode) {
    if (!shader->has_prepared()) {
      ImGui::TextColored(kColorError,
                         "ERROR: shader not prepared (not used this frame?)");
      return;
    }
  }

  switch (display_type) {
    case ShaderDisplayType::kUcode: {
      DrawMultilineString(shader->ucode_disassembly());
      break;
    }
    case ShaderDisplayType::kTranslated: {
      const auto& str = shader->translated_disassembly();
      size_t i = 0;
      bool done = false;
      while (!done && i < str.size()) {
        size_t next_i = str.find('\n', i);
        if (next_i == std::string::npos) {
          done = true;
          next_i = str.size() - 1;
        }
        auto line = str.substr(i, next_i - i);
        if (line.find("//") != std::string::npos) {
          ImGui::TextColored(kColorComment, "%s", line.c_str());
        } else {
          ImGui::Text("%s", line.c_str());
        }
        i = next_i + 1;
      }
      break;
    }
    case ShaderDisplayType::kHostDisasm: {
      DrawMultilineString(shader->host_disassembly());
      break;
    }
  }
}

// glBlendEquationSeparatei(i, blend_op, blend_op_alpha);
// glBlendFuncSeparatei(i, src_blend, dest_blend, src_blend_alpha,
//  dest_blend_alpha);
void DrawBlendMode(uint32_t src_blend, uint32_t dest_blend, uint32_t blend_op) {
  static const char* kBlendNames[] = {
      /*  0 */ "ZERO",
      /*  1 */ "ONE",
      /*  2 */ "UNK2",  // ?
      /*  3 */ "UNK3",  // ?
      /*  4 */ "SRC_COLOR",
      /*  5 */ "ONE_MINUS_SRC_COLOR",
      /*  6 */ "SRC_ALPHA",
      /*  7 */ "ONE_MINUS_SRC_ALPHA",
      /*  8 */ "DST_COLOR",
      /*  9 */ "ONE_MINUS_DST_COLOR",
      /* 10 */ "DST_ALPHA",
      /* 11 */ "ONE_MINUS_DST_ALPHA",
      /* 12 */ "CONSTANT_COLOR",
      /* 13 */ "ONE_MINUS_CONSTANT_COLOR",
      /* 14 */ "CONSTANT_ALPHA",
      /* 15 */ "ONE_MINUS_CONSTANT_ALPHA",
      /* 16 */ "SRC_ALPHA_SATURATE",
  };
  const char* src_str = kBlendNames[src_blend];
  const char* dest_str = kBlendNames[dest_blend];
  const char* op_template;
  switch (blend_op) {
    case 0:  // add
      op_template = "%s + %s";
      break;
    case 1:  // subtract
      op_template = "%s - %s";
      break;
    case 2:  // min
      op_template = "min(%s, %s)";
      break;
    case 3:  // max
      op_template = "max(%s, %s)";
      break;
    case 4:  // reverse subtract
      op_template = "-(%s) + %s";
      break;
    default:
      op_template = "%s ? %s";
      break;
  }
  ImGui::Text(op_template, src_str, dest_str);
}

void DrawFailedTextureInfo(const Shader::SamplerDesc& desc,
                           const char* message) {
  // TODO(benvanik): better error info/etc.
  ImGui::TextColored(kColorError, "ERROR: %s", message);
}
void DrawTextureInfo(TracePlayer& player, const Shader::SamplerDesc& desc) {
  auto gs = static_cast<gl4::GL4GraphicsSystem*>(player.graphics_system());
  auto cp = gs->command_processor();
  auto& regs = *gs->register_file();

  int r = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + desc.fetch_slot * 6;
  auto group = reinterpret_cast<const xe_gpu_fetch_group_t*>(&regs.values[r]);
  auto& fetch = group->texture_fetch;
  if (fetch.type != 0x2) {
    DrawFailedTextureInfo(desc, "Invalid fetch type");
    return;
  }
  TextureInfo texture_info;
  if (!TextureInfo::Prepare(fetch, &texture_info)) {
    DrawFailedTextureInfo(desc, "Unable to parse texture fetcher info");
    return;
  }
  SamplerInfo sampler_info;
  if (!SamplerInfo::Prepare(fetch, desc.tex_fetch, &sampler_info)) {
    DrawFailedTextureInfo(desc, "Unable to parse sampler info");
    return;
  }
  auto entry_view = cp->texture_cache()->Demand(texture_info, sampler_info);
  if (!entry_view) {
    DrawFailedTextureInfo(desc, "Failed to demand texture");
    return;
  }
  auto texture = entry_view->texture;

  ImGui::Columns(2);
  ImVec2 button_size(256, 256);
  if (ImGui::ImageButton(ImTextureID(GLuint64(texture->handle)), button_size,
                         ImVec2(0, 0), ImVec2(1, 1))) {
    // show viewer
  }
  ImGui::NextColumn();
  ImGui::Text("Fetch Slot: %d", desc.fetch_slot);
  switch (texture_info.dimension) {
    case Dimension::k1D:
      ImGui::Text("1D: %dpx", texture_info.width + 1);
      break;
    case Dimension::k2D:
      ImGui::Text("2D: %dx%dpx", texture_info.width + 1,
                  texture_info.height + 1);
      break;
    case Dimension::k3D:
      ImGui::Text("3D: %dx%dx%dpx", texture_info.width + 1,
                  texture_info.height + 1, texture_info.depth + 1);
      break;
    case Dimension::kCube:
      ImGui::Text("Cube: ?");
      break;
  }
  ImGui::Columns(1);
}

void DrawStateUI(xe::ui::MainWindow* window, TracePlayer& player,
                 uint8_t* membase) {
  auto gs = static_cast<gl4::GL4GraphicsSystem*>(player.graphics_system());
  auto cp = gs->command_processor();
  auto& regs = *gs->register_file();

  ImGui::SetNextWindowPos(ImVec2(float(window->width()) - 500 - 5, 30),
                          ImGuiSetCondition_FirstUseEver);
  if (!ImGui::Begin("State", nullptr, ImVec2(500, 680))) {
    ImGui::End();
    return;
  }

  if (!player.current_frame() || player.current_command_index() == -1) {
    ImGui::Text("No frame/command selected");
    ImGui::End();
    return;
  }

  auto frame = player.current_frame();
  const auto& command = frame->commands[player.current_command_index()];
  auto packet_head = command.head_ptr + sizeof(PacketStartCommand);
  uint32_t packet = poly::load_and_swap<uint32_t>(packet_head);
  uint32_t packet_type = packet >> 30;
  assert_true(packet_type == 0x03);
  uint32_t opcode = (packet >> 8) & 0x7F;
  struct {
    PrimitiveType prim_type;
    bool is_auto_index;
    uint32_t index_count;
    uint32_t index_buffer_ptr;
    uint32_t index_buffer_size;
    Endian index_endianness;
    IndexFormat index_format;
  } draw_info;
  std::memset(&draw_info, 0, sizeof(draw_info));
  switch (opcode) {
    case PM4_DRAW_INDX: {
      uint32_t dword0 = poly::load_and_swap<uint32_t>(packet_head + 4);
      uint32_t dword1 = poly::load_and_swap<uint32_t>(packet_head + 8);
      draw_info.index_count = dword1 >> 16;
      draw_info.prim_type = static_cast<PrimitiveType>(dword1 & 0x3F);
      uint32_t src_sel = (dword1 >> 6) & 0x3;
      if (src_sel == 0x0) {
        // Indexed draw.
        draw_info.is_auto_index = false;
        draw_info.index_buffer_ptr =
            poly::load_and_swap<uint32_t>(packet_head + 12);
        uint32_t index_size = poly::load_and_swap<uint32_t>(packet_head + 16);
        draw_info.index_endianness = static_cast<Endian>(index_size >> 30);
        index_size &= 0x00FFFFFF;
        bool index_32bit = (dword1 >> 11) & 0x1;
        draw_info.index_format =
            index_32bit ? IndexFormat::kInt32 : IndexFormat::kInt16;
        draw_info.index_buffer_size = index_size * (index_32bit ? 4 : 2);
      } else if (src_sel == 0x2) {
        // Auto draw.
        draw_info.is_auto_index = true;
      } else {
        // Unknown source select.
        assert_always();
      }
      break;
    }
    case PM4_DRAW_INDX_2: {
      uint32_t dword0 = poly::load_and_swap<uint32_t>(packet_head + 4);
      uint32_t src_sel = (dword0 >> 6) & 0x3;
      assert_true(src_sel == 0x2);  // 'SrcSel=AutoIndex'
      draw_info.prim_type = static_cast<PrimitiveType>(dword0 & 0x3F);
      draw_info.is_auto_index = true;
      draw_info.index_count = dword0 >> 16;
      break;
    }
  }

  auto enable_mode =
      static_cast<ModeControl>(regs[XE_GPU_REG_RB_MODECONTROL].u32 & 0x7);
  const char* mode_name = "Unknown";
  switch (enable_mode) {
    case ModeControl::kIgnore:
      mode_name = "Ignored";
      break;
    case ModeControl::kColorDepth:
      mode_name = "Color + Depth";
      break;
    case ModeControl::kDepth:
      mode_name = "Depth-only";
      break;
    case ModeControl::kCopy:
      mode_name = "Copy";
      break;
  }
  ImGui::Text("%s Command %d", mode_name, player.current_command_index());
  static const char* kPrimNames[] = {
      "<none>",         "point list",   "line list",      "line strip",
      "triangle list",  "triangle fan", "triangle strip", "unknown 0x7",
      "rectangle list", "unknown 0x9",  "unknown 0xA",    "unknown 0xB",
      "line loop",      "quad list",    "quad strip",     "unknown 0xF",
  };
  ImGui::Text("Primitive Type: %s", kPrimNames[int(draw_info.prim_type)]);
  ImGui::Text("Indices: %d", draw_info.index_count);
  ImGui::SameLine();
  if (draw_info.is_auto_index) {
    ImGui::Text("auto-indexed");
  } else {
    static const char* kIndexFormatNames[] = {
        "uint16", "uint32",
    };
    static const char* kEndiannessNames[] = {
        "unspecified endianness", "8-in-16", "8-in-32", "16-in-32",
    };
    ImGui::Text("from buffer %.8X (%db), %s, %s", draw_info.index_buffer_ptr,
                draw_info.index_buffer_size,
                kIndexFormatNames[int(draw_info.index_format)],
                kEndiannessNames[int(draw_info.index_endianness)]);
  }

  if (ImGui::TreeNode("Viewport State")) {
    uint32_t pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL].u32;
    if ((pa_su_sc_mode_cntl >> 17) & 1) {
      uint32_t window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET].u32;
      ImGui::BulletText("Window Offset: %d,%d", window_offset & 0x7FFF,
                        (window_offset >> 16) & 0x7FFF);
    } else {
      ImGui::BulletText("Window Offset: disabled");
    }
    uint32_t window_scissor_tl = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL].u32;
    uint32_t window_scissor_br = regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR].u32;
    ImGui::BulletText(
        "Window Scissor: %d,%d to %d,%d (%d x %d)", window_scissor_tl & 0x7FFF,
        (window_scissor_tl >> 16) & 0x7FFF, window_scissor_br & 0x7FFF,
        (window_scissor_br >> 16) & 0x7FFF,
        (window_scissor_br & 0x7FFF) - (window_scissor_tl & 0x7FFF),
        ((window_scissor_br >> 16) & 0x7FFF) -
            ((window_scissor_tl >> 16) & 0x7FFF));
    uint32_t surface_info = regs[XE_GPU_REG_RB_SURFACE_INFO].u32;
    uint32_t surface_pitch = surface_info & 0x3FFF;
    auto surface_msaa = (surface_info >> 16) & 0x3;
    static const char* kMsaaNames[] = {
        "1X", "2X", "4X",
    };
    ImGui::BulletText("Surface MSAA: %s", kMsaaNames[surface_msaa]);
    uint32_t vte_control = regs[XE_GPU_REG_PA_CL_VTE_CNTL].u32;
    bool vport_xscale_enable = (vte_control & (1 << 0)) > 0;
    bool vport_xoffset_enable = (vte_control & (1 << 1)) > 0;
    bool vport_yscale_enable = (vte_control & (1 << 2)) > 0;
    bool vport_yoffset_enable = (vte_control & (1 << 3)) > 0;
    bool vport_zscale_enable = (vte_control & (1 << 4)) > 0;
    bool vport_zoffset_enable = (vte_control & (1 << 5)) > 0;
    assert_true(vport_xscale_enable == vport_yscale_enable ==
                vport_zscale_enable == vport_xoffset_enable ==
                vport_yoffset_enable == vport_zoffset_enable);
    ImGui::BulletText(
        "Viewport Offset: %f, %f, %f",
        vport_xoffset_enable ? regs[XE_GPU_REG_PA_CL_VPORT_XOFFSET].f32 : 0,
        vport_yoffset_enable ? regs[XE_GPU_REG_PA_CL_VPORT_YOFFSET].f32 : 0,
        vport_zoffset_enable ? regs[XE_GPU_REG_PA_CL_VPORT_ZOFFSET].f32 : 0);
    ImGui::BulletText(
        "Viewport Scale: %f, %f, %f",
        vport_xscale_enable ? regs[XE_GPU_REG_PA_CL_VPORT_XSCALE].f32 : 1,
        vport_yscale_enable ? regs[XE_GPU_REG_PA_CL_VPORT_YSCALE].f32 : 1,
        vport_zscale_enable ? regs[XE_GPU_REG_PA_CL_VPORT_ZSCALE].f32 : 1);
    ImGui::BulletText("XY Vertex Format: %s", ((vte_control >> 8) & 0x1)
                                                  ? "premultipied by 1/w0"
                                                  : "unmultiplied");
    ImGui::BulletText("Z Vertex Format: %s", ((vte_control >> 9) & 0x1)
                                                 ? "premultipied by 1/w0"
                                                 : "unmultiplied");
    ImGui::BulletText("W0 Vertex Format: %s", ((vte_control >> 10) & 0x1)
                                                  ? "w0 is not 1/w0"
                                                  : "already divided");
    uint32_t clip_control = regs[XE_GPU_REG_PA_CL_CLIP_CNTL].u32;
    bool clip_enabled = ((clip_control >> 17) & 0x1) == 0;
    ImGui::BulletText("Clip Enabled: %s", clip_enabled ? "true" : "false");
    bool dx_clip = ((clip_control >> 20) & 0x1) == 0x1;
    ImGui::BulletText("DX Clip: %s", dx_clip ? "true" : "false");
    ImGui::TreePop();
  }

  if (ImGui::TreeNode("Rasterizer State")) {
    uint32_t pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL].u32;
    uint32_t pa_sc_screen_scissor_tl =
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL].u32;
    uint32_t pa_sc_screen_scissor_br =
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR].u32;
    if (pa_sc_screen_scissor_tl != 0 && pa_sc_screen_scissor_br != 0x20002000) {
      int32_t screen_scissor_x = pa_sc_screen_scissor_tl & 0x7FFF;
      int32_t screen_scissor_y = (pa_sc_screen_scissor_tl >> 16) & 0x7FFF;
      int32_t screen_scissor_w =
          (pa_sc_screen_scissor_br & 0x7FFF) - screen_scissor_x;
      int32_t screen_scissor_h =
          ((pa_sc_screen_scissor_br >> 16) & 0x7FFF) - screen_scissor_y;
      ImGui::BulletText("Scissor: %d,%d to %d,%d (%d x %d)", screen_scissor_x,
                        screen_scissor_y, screen_scissor_x + screen_scissor_w,
                        screen_scissor_y + screen_scissor_h, screen_scissor_w,
                        screen_scissor_h);
    } else {
      ImGui::BulletText("Scissor: disabled");
    }
    switch (pa_su_sc_mode_cntl & 0x3) {
      case 0:
        ImGui::BulletText("Culling: disabled");
        break;
      case 1:
        ImGui::BulletText("Culling: front-face");
        break;
      case 2:
        ImGui::BulletText("Culling: back-face");
        break;
    }
    if (pa_su_sc_mode_cntl & 0x4) {
      ImGui::BulletText("Front-face: clockwise");
    } else {
      ImGui::BulletText("Front-face: counter-clockwise");
    }
    static const char* kFillModeNames[3] = {
        "point", "line", "fill",
    };
    bool poly_mode = ((pa_su_sc_mode_cntl >> 3) & 0x3) != 0;
    if (poly_mode) {
      uint32_t front_poly_mode = (pa_su_sc_mode_cntl >> 5) & 0x7;
      uint32_t back_poly_mode = (pa_su_sc_mode_cntl >> 8) & 0x7;
      // GL only supports both matching.
      assert_true(front_poly_mode == back_poly_mode);
      ImGui::BulletText("Polygon Mode: %s", kFillModeNames[front_poly_mode]);
    } else {
      ImGui::BulletText("Polygon Mode: fill");
    }
    if (pa_su_sc_mode_cntl & (1 << 20)) {
      ImGui::BulletText("Provoking Vertex: last");
    } else {
      ImGui::BulletText("Provoking Vertex: first");
    }
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Blend State")) {
    uint32_t rb_blendcontrol[4] = {
        regs[XE_GPU_REG_RB_BLENDCONTROL_0].u32,
        regs[XE_GPU_REG_RB_BLENDCONTROL_1].u32,
        regs[XE_GPU_REG_RB_BLENDCONTROL_2].u32,
        regs[XE_GPU_REG_RB_BLENDCONTROL_3].u32,
    };
    for (int i = 0; i < poly::countof(rb_blendcontrol); ++i) {
      uint32_t blend_control = rb_blendcontrol[i];
      // A2XX_RB_BLEND_CONTROL_COLOR_SRCBLEND
      auto src_blend = (blend_control & 0x0000001F) >> 0;
      // A2XX_RB_BLEND_CONTROL_COLOR_DESTBLEND
      auto dest_blend = (blend_control & 0x00001F00) >> 8;
      // A2XX_RB_BLEND_CONTROL_COLOR_COMB_FCN
      auto blend_op = (blend_control & 0x000000E0) >> 5;
      // A2XX_RB_BLEND_CONTROL_ALPHA_SRCBLEND
      auto src_blend_alpha = (blend_control & 0x001F0000) >> 16;
      // A2XX_RB_BLEND_CONTROL_ALPHA_DESTBLEND
      auto dest_blend_alpha = (blend_control & 0x1F000000) >> 24;
      // A2XX_RB_BLEND_CONTROL_ALPHA_COMB_FCN
      auto blend_op_alpha = (blend_control & 0x00E00000) >> 21;
      // A2XX_RB_COLORCONTROL_BLEND_DISABLE ?? Can't find this!
      // Just guess based on actions.
      bool blend_enable = !((src_blend == 1) && (dest_blend == 0) &&
                            (blend_op == 0) && (src_blend_alpha == 1) &&
                            (dest_blend_alpha == 0) && (blend_op_alpha == 0));
      if (blend_enable) {
        if (src_blend == src_blend_alpha && dest_blend == dest_blend_alpha &&
            blend_op == blend_op_alpha) {
          ImGui::BulletText("Blend %d: ", i);
          ImGui::SameLine();
          DrawBlendMode(src_blend, dest_blend, blend_op);
        } else {
          ImGui::BulletText("Blend %d:", i);
          ImGui::BulletText("  Color: ");
          ImGui::SameLine();
          DrawBlendMode(src_blend, dest_blend, blend_op);
          ImGui::BulletText("  Alpha: ");
          ImGui::SameLine();
          DrawBlendMode(src_blend_alpha, dest_blend_alpha, blend_op_alpha);
        }
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kColorIgnored);
        ImGui::BulletText("Blend %d: disabled", i);
        ImGui::PopStyleColor();
      }
    }
    auto blend_color = ImVec4(regs[XE_GPU_REG_RB_BLEND_RED].f32,
                              regs[XE_GPU_REG_RB_BLEND_GREEN].f32,
                              regs[XE_GPU_REG_RB_BLEND_BLUE].f32,
                              regs[XE_GPU_REG_RB_BLEND_ALPHA].f32);
    ImGui::BulletText("Blend Color: (%.2f,%.2f,%.2f,%.2f)", blend_color.x,
                      blend_color.y, blend_color.z, blend_color.w);
    ImGui::SameLine();
    ImGui::ColorButton(blend_color, true);
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Depth-Stencil State")) {
    auto rb_depthcontrol = regs[XE_GPU_REG_RB_DEPTHCONTROL].u32;
    auto rb_stencilrefmask = regs[XE_GPU_REG_RB_STENCILREFMASK].u32;
    static const char* kCompareFuncNames[] = {
        "<false>", "<", "==", "<=", ">", "!=", ">=", "<true>",
    };
    if (rb_depthcontrol & 0x00000002) {
      ImGui::BulletText("Depth Test: enabled");
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kColorIgnored);
      ImGui::BulletText("Depth Test: disabled");
    }
    ImGui::BulletText("Depth Func: %s",
                      kCompareFuncNames[(rb_depthcontrol & 0x00000070) >> 4]);
    if (!(rb_depthcontrol & 0x00000002)) {
      ImGui::PopStyleColor();
    }
    if (rb_depthcontrol & 0x00000004) {
      ImGui::BulletText("Depth Write: enabled");
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kColorIgnored);
      ImGui::BulletText("Depth Write: disabled");
      ImGui::PopStyleColor();
    }
    if (rb_depthcontrol & 0x00000001) {
      ImGui::BulletText("Stencil Test: enabled");
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kColorIgnored);
      ImGui::BulletText("Stencil Test: disabled");
    }
    // TODO(benvanik): stencil stuff.
    ImGui::BulletText("TODO: stencil stuff");
    if (!(rb_depthcontrol & 0x00000001)) {
      ImGui::PopStyleColor();
    }
    ImGui::TreePop();
  }

  if (ImGui::CollapsingHeader("Vertex Shader")) {
    ShaderDisplayType shader_display_type = DrawShaderTypeUI();
    ImGui::BeginChild("#vertex_shader_text", ImVec2(0, 400));
    auto shader = cp->active_vertex_shader();
    if (shader) {
      DrawShaderUI(window, player, membase, shader, shader_display_type);
    } else {
      ImGui::TextColored(kColorError, "ERROR: no vertex shader set");
    }
    ImGui::EndChild();
  }
  if (ImGui::CollapsingHeader("Pixel Shader")) {
    ShaderDisplayType shader_display_type = DrawShaderTypeUI();
    ImGui::BeginChild("#pixel_shader_text", ImVec2(0, 400));
    auto shader = cp->active_pixel_shader();
    if (shader) {
      DrawShaderUI(window, player, membase, shader, shader_display_type);
    } else {
      ImGui::TextColored(kColorError, "ERROR: no pixel shader set");
    }
    ImGui::EndChild();
  }
  if (ImGui::CollapsingHeader("Buffers")) {
    ImGui::Text("vertex/index buffers");
  }
  if (ImGui::CollapsingHeader("Vertex Textures")) {
    auto shader = cp->active_vertex_shader();
    if (shader) {
      const auto& sampler_inputs = shader->sampler_inputs();
      if (sampler_inputs.count) {
        for (size_t i = 0; i < sampler_inputs.count; ++i) {
          DrawTextureInfo(player, sampler_inputs.descs[i]);
        }
      } else {
        ImGui::Text("No vertex shader samplers");
      }
    } else {
      ImGui::TextColored(kColorError, "ERROR: no vertex shader set");
    }
  }
  if (ImGui::CollapsingHeader("Textures")) {
    auto shader = cp->active_pixel_shader();
    if (shader) {
      const auto& sampler_inputs = shader->sampler_inputs();
      if (sampler_inputs.count) {
        for (size_t i = 0; i < sampler_inputs.count; ++i) {
          DrawTextureInfo(player, sampler_inputs.descs[i]);
        }
      } else {
        ImGui::Text("No pixel shader samplers");
      }
    } else {
      ImGui::TextColored(kColorError, "ERROR: no pixel shader set");
    }
  }
  if (ImGui::CollapsingHeader("Fetch Constants (raw)")) {
    ImGui::Columns(2);
    ImGui::SetColumnOffset(1, 85.0f);
    for (int i = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
         i <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5; ++i) {
      ImGui::Text("f%02d_%d", (i - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 6,
                  (i - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) % 6);
      ImGui::NextColumn();
      ImGui::Text("%.8X", regs[i].u32);
      ImGui::NextColumn();
    }
    ImGui::Columns(1);
  }
  if (ImGui::CollapsingHeader("ALU Constants")) {
    ImGui::Columns(2);
    for (int i = XE_GPU_REG_SHADER_CONSTANT_000_X;
         i <= XE_GPU_REG_SHADER_CONSTANT_511_X; i += 4) {
      ImGui::Text("c%d", (i - XE_GPU_REG_SHADER_CONSTANT_000_X) / 4);
      ImGui::NextColumn();
      ImGui::Text("%f, %f, %f, %f", regs[i + 0].f32, regs[i + 1].f32,
                  regs[i + 2].f32, regs[i + 3].f32);
      ImGui::NextColumn();
    }
    ImGui::Columns(1);
  }
  if (ImGui::CollapsingHeader("Bool Constants")) {
    ImGui::Columns(2);
    for (int i = XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031;
         i <= XE_GPU_REG_SHADER_CONSTANT_BOOL_224_255; ++i) {
      ImGui::Text("b%03d-%03d",
                  (i - XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) * 32,
                  (i - XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) * 32 + 31);
      ImGui::NextColumn();
      ImGui::Text("%.8X", regs[i].u32);
      ImGui::NextColumn();
    }
    ImGui::Columns(1);
  }
  if (ImGui::CollapsingHeader("Loop Constants")) {
    ImGui::Columns(2);
    for (int i = XE_GPU_REG_SHADER_CONSTANT_LOOP_00;
         i <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31; ++i) {
      ImGui::Text("l%d", i - XE_GPU_REG_SHADER_CONSTANT_LOOP_00);
      ImGui::NextColumn();
      ImGui::Text("%.8X", regs[i].u32);
      ImGui::NextColumn();
    }
    ImGui::Columns(1);
  }
  ImGui::End();
}

void DrawPacketDisassemblerUI(xe::ui::MainWindow* window, TracePlayer& player,
                              uint8_t* membase) {
  ImGui::SetNextWindowCollapsed(true, ImGuiSetCondition_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(float(window->width()) - 500 - 5, 5),
                          ImGuiSetCondition_FirstUseEver);
  if (!ImGui::Begin("Packet Disassembler", nullptr, ImVec2(500, 300))) {
    ImGui::End();
    return;
  }
  if (!player.current_frame() || player.current_command_index() == -1) {
    ImGui::Text("No frame/command selected");
    ImGui::End();
    return;
  }

  auto frame = player.current_frame();
  const auto& command = frame->commands[player.current_command_index()];
  const uint8_t* start_ptr = command.start_ptr;
  const uint8_t* end_ptr = command.end_ptr;

  ImGui::Text("Frame #%d, command %d", player.current_frame_index(),
              player.current_command_index());
  ImGui::Separator();
  ImGui::BeginChild("packet_disassembler_list");
  const PacketStartCommand* pending_packet = nullptr;
  auto trace_ptr = start_ptr;
  while (trace_ptr < end_ptr) {
    auto type = static_cast<TraceCommandType>(poly::load<uint32_t>(trace_ptr));
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart: {
        auto cmd =
            reinterpret_cast<const PrimaryBufferStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        ImGui::BulletText("PrimaryBufferStart");
        break;
      }
      case TraceCommandType::kPrimaryBufferEnd: {
        auto cmd = reinterpret_cast<const PrimaryBufferEndCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        ImGui::BulletText("PrimaryBufferEnd");
        break;
      }
      case TraceCommandType::kIndirectBufferStart: {
        auto cmd =
            reinterpret_cast<const IndirectBufferStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        ImGui::BulletText("IndirectBufferStart");
        break;
      }
      case TraceCommandType::kIndirectBufferEnd: {
        auto cmd = reinterpret_cast<const IndirectBufferEndCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        ImGui::BulletText("IndirectBufferEnd");
        break;
      }
      case TraceCommandType::kPacketStart: {
        auto cmd = reinterpret_cast<const PacketStartCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->count * 4;
        pending_packet = cmd;
        break;
      }
      case TraceCommandType::kPacketEnd: {
        auto cmd = reinterpret_cast<const PacketEndCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        if (pending_packet) {
          PacketInfo packet_info;
          if (DisasmPacket(reinterpret_cast<const uint8_t*>(pending_packet) +
                               sizeof(PacketStartCommand),
                           &packet_info)) {
            ImGui::BulletText(packet_info.type_info->name);
            ImGui::TreePush((const char*)0);
            for (auto& action : packet_info.actions) {
              switch (action.type) {
                case PacketAction::Type::kRegisterWrite: {
                  auto register_info = xe::gpu::RegisterFile::GetRegisterInfo(
                      action.register_write.index);
                  ImGui::Columns(2);
                  ImGui::Text("%.4X %s", action.register_write.index,
                              register_info ? register_info->name : "???");
                  ImGui::NextColumn();
                  if (!register_info ||
                      register_info->type == RegisterInfo::Type::kDword) {
                    ImGui::Text("%.8X", action.register_write.value.u32);
                  } else {
                    ImGui::Text("%8f", action.register_write.value.f32);
                  }
                  ImGui::Columns(1);
                  break;
                }
              }
            }
            ImGui::TreePop();
          } else {
            ImGui::BulletText("<invalid packet>");
          }
          pending_packet = nullptr;
        }
        break;
      }
      case TraceCommandType::kMemoryRead: {
        auto cmd = reinterpret_cast<const MemoryReadCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->length;
        // ImGui::BulletText("MemoryRead");
        break;
      }
      case TraceCommandType::kMemoryWrite: {
        auto cmd = reinterpret_cast<const MemoryWriteCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd) + cmd->length;
        // ImGui::BulletText("MemoryWrite");
        break;
      }
      case TraceCommandType::kEvent: {
        auto cmd = reinterpret_cast<const EventCommand*>(trace_ptr);
        trace_ptr += sizeof(*cmd);
        switch (cmd->event_type) {
          case EventType::kSwap: {
            ImGui::BulletText("<swap>");
            break;
          }
        }
        break;
      }
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

void DrawUI(xe::ui::MainWindow* window, TracePlayer& player, uint8_t* membase) {
  ImGui::ShowTestWindow();

  DrawControllerUI(window, player, membase);
  DrawCommandListUI(window, player, membase);
  DrawStateUI(window, player, membase);
  DrawPacketDisassemblerUI(window, player, membase);
}

void ImImpl_Setup();
void ImImpl_Shutdown();

int trace_viewer_main(std::vector<std::wstring>& args) {
  // Create the emulator.
  auto emulator = std::make_unique<Emulator>(L"");
  X_STATUS result = emulator->Setup();
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: %.8X", result);
    return 1;
  }

  // Grab path from the flag or unnamed argument.
  if (!FLAGS_target_trace_file.empty() || args.size() >= 2) {
    std::wstring path;
    if (!FLAGS_target_trace_file.empty()) {
      // Passed as a named argument.
      // TODO(benvanik): find something better than gflags that supports
      // unicode.
      path = poly::to_wstring(FLAGS_target_trace_file);
    } else {
      // Passed as an unnamed argument.
      path = args[1];
    }
    // Normalize the path and make absolute.
    auto abs_path = poly::to_absolute_path(path);

    auto window = emulator->main_window();
    auto loop = window->loop();
    auto file_name = poly::find_name_from_path(path);
    window->set_title(std::wstring(L"Xenia GPU Trace Viewer: ") + file_name);

    auto graphics_system = emulator->graphics_system();
    Profiler::set_display(nullptr);

    TracePlayer player(loop, emulator->graphics_system());
    if (!player.Open(abs_path)) {
      XELOGE("Could not load trace file");
      return 1;
    }

    auto control = window->child(0);
    control->on_key_char.AddListener([](poly::ui::KeyEvent& e) {
      auto& io = ImGui::GetIO();
      if (e.key_code() > 0 && e.key_code() < 0x10000) {
        io.AddInputCharacter(e.key_code());
      }
      e.set_handled(true);
    });
    control->on_mouse_down.AddListener([](poly::ui::MouseEvent& e) {
      auto& io = ImGui::GetIO();
      io.MousePos = ImVec2(float(e.x()), float(e.y()));
      switch (e.button()) {
        case poly::ui::MouseEvent::Button::kLeft:
          io.MouseDown[0] = true;
          break;
        case poly::ui::MouseEvent::Button::kRight:
          io.MouseDown[1] = true;
          break;
      }
    });
    control->on_mouse_move.AddListener([](poly::ui::MouseEvent& e) {
      auto& io = ImGui::GetIO();
      io.MousePos = ImVec2(float(e.x()), float(e.y()));
    });
    control->on_mouse_up.AddListener([](poly::ui::MouseEvent& e) {
      auto& io = ImGui::GetIO();
      io.MousePos = ImVec2(float(e.x()), float(e.y()));
      switch (e.button()) {
        case poly::ui::MouseEvent::Button::kLeft:
          io.MouseDown[0] = false;
          break;
        case poly::ui::MouseEvent::Button::kRight:
          io.MouseDown[1] = false;
          break;
      }
    });
    control->on_mouse_wheel.AddListener([](poly::ui::MouseEvent& e) {
      auto& io = ImGui::GetIO();
      io.MousePos = ImVec2(float(e.x()), float(e.y()));
      io.MouseWheel += float(e.dy() / 120.0f);
    });

    control->on_paint.AddListener([&](poly::ui::UIEvent& e) {
      static bool imgui_setup = false;
      if (!imgui_setup) {
        ImImpl_Setup();
        imgui_setup = true;
      }
      auto& io = ImGui::GetIO();
      auto current_ticks = poly::threading::ticks();
      static uint64_t last_ticks = 0;
      io.DeltaTime = (current_ticks - last_ticks) /
                     float(poly::threading::ticks_per_second());
      last_ticks = current_ticks;

      io.DisplaySize =
          ImVec2(float(e.control()->width()), float(e.control()->height()));

      BYTE keystate[256];
      GetKeyboardState(keystate);
      for (int i = 0; i < 256; i++) io.KeysDown[i] = (keystate[i] & 0x80) != 0;
      io.KeyCtrl = (keystate[VK_CONTROL] & 0x80) != 0;
      io.KeyShift = (keystate[VK_SHIFT] & 0x80) != 0;

      ImGui::NewFrame();

      DrawUI(window, player, emulator->memory()->membase());

      glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
      ImGui::Render();

      graphics_system->RequestSwap();
    });
    graphics_system->RequestSwap();

    // Wait until we are exited.
    emulator->main_window()->loop()->AwaitQuit();

    ImImpl_Shutdown();
  }

  emulator.reset();
  return 0;
}

// TODO(benvanik): move to another file.

extern "C" GLEWContext* glewGetContext();
extern "C" WGLEWContext* wglewGetContext();

static int shader_handle, vert_handle, frag_handle;
static int texture_location, proj_mtx_location;
static int position_location, uv_location, colour_location;
static size_t vbo_max_size = 20000;
static unsigned int vbo_handle, vao_handle;
void ImImpl_RenderDrawLists(ImDrawList** const cmd_lists, int cmd_lists_count);
void ImImpl_Setup() {
  ImGuiIO& io = ImGui::GetIO();

  const GLchar* vertex_shader =
      "#version 330\n"
      "uniform mat4 ProjMtx;\n"
      "in vec2 Position;\n"
      "in vec2 UV;\n"
      "in vec4 Color;\n"
      "out vec2 Frag_UV;\n"
      "out vec4 Frag_Color;\n"
      "void main()\n"
      "{\n"
      "	Frag_UV = UV;\n"
      "	Frag_Color = Color;\n"
      "	gl_Position = ProjMtx * vec4(Position.xy,0,1);\n"
      "}\n";

  const GLchar* fragment_shader =
      "#version 330\n"
      "uniform sampler2D Texture;\n"
      "in vec2 Frag_UV;\n"
      "in vec4 Frag_Color;\n"
      "out vec4 Out_Color;\n"
      "void main()\n"
      "{\n"
      "	Out_Color = Frag_Color * texture( Texture, Frag_UV.st);\n"
      "}\n";

  shader_handle = glCreateProgram();
  vert_handle = glCreateShader(GL_VERTEX_SHADER);
  frag_handle = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(vert_handle, 1, &vertex_shader, 0);
  glShaderSource(frag_handle, 1, &fragment_shader, 0);
  glCompileShader(vert_handle);
  glCompileShader(frag_handle);
  glAttachShader(shader_handle, vert_handle);
  glAttachShader(shader_handle, frag_handle);
  glLinkProgram(shader_handle);

  texture_location = glGetUniformLocation(shader_handle, "Texture");
  proj_mtx_location = glGetUniformLocation(shader_handle, "ProjMtx");
  position_location = glGetAttribLocation(shader_handle, "Position");
  uv_location = glGetAttribLocation(shader_handle, "UV");
  colour_location = glGetAttribLocation(shader_handle, "Color");

  glGenBuffers(1, &vbo_handle);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_handle);
  glBufferData(GL_ARRAY_BUFFER, vbo_max_size, NULL, GL_DYNAMIC_DRAW);

  glGenVertexArrays(1, &vao_handle);
  glBindVertexArray(vao_handle);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_handle);
  glEnableVertexAttribArray(position_location);
  glEnableVertexAttribArray(uv_location);
  glEnableVertexAttribArray(colour_location);

  glVertexAttribPointer(position_location, 2, GL_FLOAT, GL_FALSE,
                        sizeof(ImDrawVert), (GLvoid*)offsetof(ImDrawVert, pos));
  glVertexAttribPointer(uv_location, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
                        (GLvoid*)offsetof(ImDrawVert, uv));
  glVertexAttribPointer(colour_location, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                        sizeof(ImDrawVert), (GLvoid*)offsetof(ImDrawVert, col));
  glBindVertexArray(0);
  glDisableVertexAttribArray(position_location);
  glDisableVertexAttribArray(uv_location);
  glDisableVertexAttribArray(colour_location);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(
      &pixels, &width, &height);  // Load as RGBA 32-bits for OpenGL3 demo
  // because it is more likely to be compatible
  // with user's existing shader.

  GLuint tex_id;
  glCreateTextures(GL_TEXTURE_2D, 1, &tex_id);
  glTextureParameteri(tex_id, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTextureParameteri(tex_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTextureStorage2D(tex_id, 1, GL_RGBA8, width, height);
  glTextureSubImage2D(tex_id, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                      pixels);

  // Store our identifier
  io.Fonts->TexID = (void*)(intptr_t)tex_id;

  io.DeltaTime = 1.0f / 60.0f;
  io.RenderDrawListsFn = ImImpl_RenderDrawLists;

  auto& style = ImGui::GetStyle();
  style.WindowRounding = 0;

  style.Colors[ImGuiCol_Text] = ImVec4(0.89f, 0.90f, 0.90f, 1.00f);
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_ChildWindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style.Colors[ImGuiCol_Border] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.60f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.22f);
  style.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 1.00f, 0.00f, 0.78f);
  style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.58f, 0.00f, 0.61f);
  style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.40f, 0.11f, 0.59f);
  style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.00f, 0.68f, 0.00f, 0.68f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] =
      ImVec4(0.00f, 1.00f, 0.15f, 0.62f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] =
      ImVec4(0.00f, 0.91f, 0.09f, 0.40f);
  style.Colors[ImGuiCol_ComboBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.99f);
  style.Colors[ImGuiCol_CheckHovered] = ImVec4(0.23f, 0.64f, 0.13f, 0.45f);
  style.Colors[ImGuiCol_CheckActive] = ImVec4(0.21f, 0.93f, 0.13f, 0.55f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.74f, 0.90f, 0.72f, 0.50f);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
  style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.34f, 0.75f, 0.11f, 1.00f);
  style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.56f, 0.11f, 0.60f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.72f, 0.09f, 1.00f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.19f, 0.60f, 0.09f, 1.00f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.00f, 0.40f, 0.00f, 0.71f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.60f, 0.26f, 0.80f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.00f, 0.75f, 0.00f, 0.80f);
  style.Colors[ImGuiCol_Column] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  style.Colors[ImGuiCol_ColumnHovered] = ImVec4(0.36f, 0.89f, 0.38f, 1.00f);
  style.Colors[ImGuiCol_ColumnActive] = ImVec4(0.13f, 0.50f, 0.11f, 1.00f);
  style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
  style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
  style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
  style.Colors[ImGuiCol_CloseButton] = ImVec4(0.00f, 0.72f, 0.00f, 0.96f);
  style.Colors[ImGuiCol_CloseButtonHovered] =
      ImVec4(0.38f, 1.00f, 0.42f, 0.60f);
  style.Colors[ImGuiCol_CloseButtonActive] = ImVec4(0.56f, 1.00f, 0.64f, 1.00f);
  style.Colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogramHovered] =
      ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 0.00f, 1.00f, 0.35f);
  style.Colors[ImGuiCol_TooltipBg] = ImVec4(0.05f, 0.05f, 0.10f, 0.90f);

  io.KeyMap[ImGuiKey_Tab] = VK_TAB;
  io.KeyMap[ImGuiKey_LeftArrow] = VK_LEFT;
  io.KeyMap[ImGuiKey_RightArrow] = VK_RIGHT;
  io.KeyMap[ImGuiKey_UpArrow] = VK_UP;
  io.KeyMap[ImGuiKey_DownArrow] = VK_UP;
  io.KeyMap[ImGuiKey_Home] = VK_HOME;
  io.KeyMap[ImGuiKey_End] = VK_END;
  io.KeyMap[ImGuiKey_Delete] = VK_DELETE;
  io.KeyMap[ImGuiKey_Backspace] = VK_BACK;
  io.KeyMap[ImGuiKey_Enter] = VK_RETURN;
  io.KeyMap[ImGuiKey_Escape] = VK_ESCAPE;
  io.KeyMap[ImGuiKey_A] = 'A';
  io.KeyMap[ImGuiKey_C] = 'C';
  io.KeyMap[ImGuiKey_V] = 'V';
  io.KeyMap[ImGuiKey_X] = 'X';
  io.KeyMap[ImGuiKey_Y] = 'Y';
  io.KeyMap[ImGuiKey_Z] = 'Z';
}
void ImImpl_Shutdown() {
  ImGuiIO& io = ImGui::GetIO();
  if (vao_handle) glDeleteVertexArrays(1, &vao_handle);
  if (vbo_handle) glDeleteBuffers(1, &vbo_handle);
  glDetachShader(shader_handle, vert_handle);
  glDetachShader(shader_handle, frag_handle);
  glDeleteShader(vert_handle);
  glDeleteShader(frag_handle);
  glDeleteProgram(shader_handle);
  auto tex_id = static_cast<GLuint>(intptr_t(io.Fonts->TexID));
  glDeleteTextures(1, &tex_id);
  ImGui::Shutdown();
}
void ImImpl_RenderDrawLists(ImDrawList** const cmd_lists, int cmd_lists_count) {
  if (cmd_lists_count == 0) return;

  // Setup render state: alpha-blending enabled, no face culling, no depth
  // testing, scissor enabled
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glActiveTexture(GL_TEXTURE0);

  // Setup orthographic projection matrix
  const float width = ImGui::GetIO().DisplaySize.x;
  const float height = ImGui::GetIO().DisplaySize.y;
  const float ortho_projection[4][4] = {
      {2.0f / width, 0.0f, 0.0f, 0.0f},
      {0.0f, 2.0f / -height, 0.0f, 0.0f},
      {0.0f, 0.0f, -1.0f, 0.0f},
      {-1.0f, 1.0f, 0.0f, 1.0f},
  };
  glProgramUniform1i(shader_handle, texture_location, 0);
  glProgramUniformMatrix4fv(shader_handle, proj_mtx_location, 1, GL_FALSE,
                            &ortho_projection[0][0]);

  // Grow our buffer according to what we need
  size_t total_vtx_count = 0;
  for (int n = 0; n < cmd_lists_count; n++)
    total_vtx_count += cmd_lists[n]->vtx_buffer.size();
  glBindBuffer(GL_ARRAY_BUFFER, vbo_handle);
  size_t neededBufferSize = total_vtx_count * sizeof(ImDrawVert);
  if (neededBufferSize > vbo_max_size) {
    vbo_max_size = neededBufferSize + 5000;  // Grow buffer
    glBufferData(GL_ARRAY_BUFFER, vbo_max_size, NULL, GL_STREAM_DRAW);
  }

  // Copy and convert all vertices into a single contiguous buffer
  unsigned char* buffer_data =
      (unsigned char*)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
  if (!buffer_data) return;
  for (int n = 0; n < cmd_lists_count; n++) {
    const ImDrawList* cmd_list = cmd_lists[n];
    memcpy(buffer_data, &cmd_list->vtx_buffer[0],
           cmd_list->vtx_buffer.size() * sizeof(ImDrawVert));
    buffer_data += cmd_list->vtx_buffer.size() * sizeof(ImDrawVert);
  }
  glUnmapBuffer(GL_ARRAY_BUFFER);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(vao_handle);
  glUseProgram(shader_handle);

  int cmd_offset = 0;
  ImTextureID prev_texture_id = 0;
  for (int n = 0; n < cmd_lists_count; n++) {
    const ImDrawList* cmd_list = cmd_lists[n];
    int vtx_offset = cmd_offset;
    const ImDrawCmd* pcmd_end = cmd_list->commands.end();
    for (const ImDrawCmd* pcmd = cmd_list->commands.begin(); pcmd != pcmd_end;
         pcmd++) {
      if (pcmd->texture_id != prev_texture_id) {
        glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->texture_id);
        prev_texture_id = pcmd->texture_id;
      }
      glScissor((int)pcmd->clip_rect.x, (int)(height - pcmd->clip_rect.w),
                (int)(pcmd->clip_rect.z - pcmd->clip_rect.x),
                (int)(pcmd->clip_rect.w - pcmd->clip_rect.y));
      glDrawArrays(GL_TRIANGLES, vtx_offset, pcmd->vtx_count);
      vtx_offset += pcmd->vtx_count;
    }
    cmd_offset = vtx_offset;
  }

  // Restore modified state
  glBindVertexArray(0);
  glUseProgram(0);
  glDisable(GL_SCISSOR_TEST);
  glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace gpu
}  // namespace xe

DEFINE_ENTRY_POINT(L"gpu_trace_viewer", L"gpu_trace_viewer some.trace",
                   xe::gpu::trace_viewer_main);
