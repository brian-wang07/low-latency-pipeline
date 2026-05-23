#include <atomic>
#include <cstdint>
#include <endian.h>
#include <fcntl.h>
#include <iostream>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../../common/event.hpp"
#include "../../common/platform/spin_pause.hpp"
#include "../../common/spsc_ring.hpp"
#include "itch_event_types.hpp"
#include "itch_parser.hpp"

ItchParser::ItchParser(int fd, exchange::ExchangeRing *ring) noexcept
    : fd_(fd), ring_(ring) {

  fstat(fd_, &fs_);
  data_ = (const uint8_t *)mmap(nullptr, fs_.st_size, PROT_READ, MAP_PRIVATE,
                                fd_, 0);

  if (madvise((void *)data_, fs_.st_size, MADV_SEQUENTIAL) != 0) {
    std::cerr << "Madvise Failed" << std::endl;
  }

  cursor_ = data_;
  end_ = cursor_ + fs_.st_size;
}

void ItchParser::next() {
  uint16_t frame_length_be;
  memcpy(&frame_length_be, cursor_, sizeof(uint16_t));
  uint16_t frame_length = __builtin_bswap16(frame_length_be);

  uint8_t msg_type = *(cursor_ + 2);
  cursor_ += 3;

  uint64_t tail = ring_->tail.load(std::memory_order_relaxed);
  while (tail - ring_->head.load(std::memory_order_acquire) >=
         exchange::EXCHANGE_RING_CAPACITY) {
    SPIN_PAUSE();
  }

  auto *slot = &ring_->slots[tail & ring_->MASK];

  switch (msg_type) {
  // i hate this lmfao (thank you claude)
  case 'A': {
    itch::AddOrder msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::AddOrder));
    slot->message_type = 'A';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    slot->side = (msg.buy_sell_indicator == 'B') ? common::Side::Buy
                                                 : common::Side::Sell;
    slot->shares = __builtin_bswap32(msg.shares);
    memcpy(slot->stock, msg.stock, 8);
    slot->price = __builtin_bswap32(msg.price);
    break;
  }

  case 'F': {
    itch::AddOrderWithMPID msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::AddOrderWithMPID));
    slot->message_type = 'F';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    slot->side = (msg.buy_sell_indicator == 'B') ? common::Side::Buy
                                                 : common::Side::Sell;
    slot->shares = __builtin_bswap32(msg.shares);
    memcpy(slot->stock, msg.stock, 8);
    slot->price = __builtin_bswap32(msg.price);
    break;
  }

  case 'E': {
    itch::ExecOrder msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::ExecOrder));
    slot->message_type = 'E';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    slot->shares = __builtin_bswap32(msg.executed_shares);
    slot->match_number = __builtin_bswap64(msg.match_number);
    break;
  }

  case 'C': {
    itch::ExecOrderWithMessage msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::ExecOrderWithMessage));
    slot->message_type = 'C';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    slot->shares = __builtin_bswap32(msg.executed_shares);
    slot->match_number = __builtin_bswap64(msg.match_number);
    slot->price = __builtin_bswap32(msg.execution_price);
    break;
  }

  case 'X': {
    itch::CancelOrder msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::CancelOrder));
    slot->message_type = 'X';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    slot->shares = __builtin_bswap32(msg.cancelled_shares);
    break;
  }

  case 'D': {
    itch::DeleteOrder msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::DeleteOrder));
    slot->message_type = 'D';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    break;
  }

  case 'U': {
    itch::ReplaceOrder msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::ReplaceOrder));
    slot->message_type = 'U';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.original_order_ref_number);
    slot->new_order_ref_number = __builtin_bswap64(msg.new_order_ref_number);
    slot->shares = __builtin_bswap32(msg.shares);
    slot->price = __builtin_bswap32(msg.price);
    break;
  }

  case 'P': {
    itch::TradeMessageNonCross msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::TradeMessageNonCross));
    slot->message_type = 'P';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->order_ref_number = __builtin_bswap64(msg.order_ref_number);
    slot->side = (msg.buy_sell_indicator == 'B') ? common::Side::Buy
                                                 : common::Side::Sell;
    slot->shares = __builtin_bswap32(msg.shares);
    memcpy(slot->stock, msg.stock, 8);
    slot->price = __builtin_bswap32(msg.price);
    slot->match_number = __builtin_bswap64(msg.match_number);
    break;
  }

  case 'Q': {
    itch::TradeMessageCross msg;
    memcpy(&msg, cursor_ - 1, sizeof(itch::TradeMessageCross));
    slot->message_type = 'Q';
    slot->stock_locate = __builtin_bswap16(msg.stock_locate);
    slot->tracking_number = __builtin_bswap16(msg.tracking_number);
    slot->timestamp = be48tou64h(msg.timestamp);
    slot->shares = (uint32_t)__builtin_bswap64(msg.shares);
    memcpy(slot->stock, msg.stock, 8);
    slot->price = __builtin_bswap32(msg.cross_price);
    slot->match_number = __builtin_bswap64(msg.match_number);
    break;
  }

  default: {
    cursor_ += frame_length - 1;
    return;
  }
  }

  cursor_ += frame_length - 1;
  ring_->tail.store(tail + 1, std::memory_order_release);
}