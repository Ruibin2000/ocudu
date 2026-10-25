// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/fapi/p7/builders/crc_indication_builder.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace fapi;

TEST(crc_indication_builder, valid_indication_passes)
{
  for (unsigned i = 0; i != 2; ++i) {
    crc_indication         msg;
    crc_indication_builder builder(msg);

    auto     scs        = subcarrier_spacing::kHz30;
    unsigned sfn        = 100;
    unsigned slot_index = 15;
    auto     slot       = slot_point(scs, sfn, slot_index);

    builder.set_slot(slot);

    rnti_t    rnti          = to_rnti(10);
    harq_id_t harq_id       = to_harq_id(0);
    uint8_t   tb_crc_status = 0;

    std::optional<float> ul_sinr_dB;
    ul_sinr_dB.emplace(-65);

    std::optional<phy_time_unit> timing_advance_offset;
    timing_advance_offset.emplace(phy_time_unit::from_seconds(30));

    std::optional<float> rssi_dB;
    rssi_dB.emplace(-64);

    std::optional<float> rsrp_dB;
    rsrp_dB.emplace(-100);
    bool use_dB = i;

    builder.set_pdu(rnti, harq_id, tb_crc_status, ul_sinr_dB, timing_advance_offset, rssi_dB, rsrp_dB, use_dB);

    ASSERT_EQ(slot, msg.slot);

    ASSERT_EQ(0, msg.pdu.handle);
    ASSERT_EQ(harq_id, msg.pdu.harq_id);
    ASSERT_EQ(rnti, msg.pdu.rnti);
    ASSERT_EQ(tb_crc_status, msg.pdu.tb_crc_status_ok);
    ASSERT_EQ(static_cast<int16_t>(ul_sinr_dB ? ul_sinr_dB.value() * 500.F : -32768), msg.pdu.ul_sinr_metric);
    ASSERT_EQ(timing_advance_offset ? timing_advance_offset.value() : phy_time_unit(), msg.pdu.timing_advance_offset);
    ASSERT_EQ(static_cast<uint16_t>(rssi_dB ? (rssi_dB.value() + 128.F) * 10.F : 65535), msg.pdu.rssi);
    ASSERT_EQ(static_cast<uint16_t>(rsrp_dB ? (rsrp_dB.value() + (use_dB ? 140.F : 128.F)) * 10.F : 65535),
              msg.pdu.rsrp);
  }
}

TEST(crc_indication_builder, valid_indication_with_no_metrics_passes)
{
  crc_indication         msg;
  crc_indication_builder builder(msg);

  auto     scs        = subcarrier_spacing::kHz30;
  unsigned sfn        = 100;
  unsigned slot_index = 15;
  auto     slot       = slot_point(scs, sfn, slot_index);

  builder.set_slot(slot);

  rnti_t    rnti          = to_rnti(10);
  harq_id_t harq_id       = to_harq_id(0);
  uint8_t   tb_crc_status = 0;

  std::optional<float>         ul_sinr_dB;
  std::optional<phy_time_unit> timing_advance_offset;
  std::optional<float>         rssi_dB;
  std::optional<float>         rsrp_dB;

  builder.set_pdu(rnti, harq_id, tb_crc_status, ul_sinr_dB, timing_advance_offset, rssi_dB, rsrp_dB);

  ASSERT_EQ(slot, msg.slot);

  ASSERT_EQ(0, msg.pdu.handle);
  ASSERT_EQ(harq_id, msg.pdu.harq_id);
  ASSERT_EQ(rnti, msg.pdu.rnti);
  ASSERT_EQ(tb_crc_status, msg.pdu.tb_crc_status_ok);
  ASSERT_EQ(static_cast<int16_t>(ul_sinr_dB ? ul_sinr_dB.value() * 500.F : -32768), msg.pdu.ul_sinr_metric);
  ASSERT_EQ(timing_advance_offset, msg.pdu.timing_advance_offset);
  ASSERT_EQ(static_cast<uint16_t>(rssi_dB ? (rssi_dB.value() + 128) * 10.F : 65535), msg.pdu.rssi);
  ASSERT_EQ(static_cast<uint16_t>(ul_sinr_dB ? ul_sinr_dB.value() * 500.F : 65535), msg.pdu.rsrp);
}

TEST(crc_indication_builder, falcon_rf_measured_zero_is_distinct_from_missing_sentinels)
{
  crc_indication msg;
  crc_indication_builder builder(msg);
  builder.set_pdu(to_rnti(10), to_harq_id(0), false, 0.F, {}, {}, 0.F);
  EXPECT_EQ(msg.pdu.ul_sinr_metric, 0);
  EXPECT_EQ(msg.pdu.rsrp, 1280U);
  builder.set_pdu(to_rnti(10), to_harq_id(0), false, {}, {}, {}, {});
  EXPECT_EQ(msg.pdu.ul_sinr_metric, std::numeric_limits<int16_t>::min());
  EXPECT_EQ(msg.pdu.rsrp, std::numeric_limits<uint16_t>::max());
}

TEST(crc_indication_builder, falcon_rf_wire_quantization_truncates_fractional_rsrp_and_signed_sinr)
{
  crc_indication msg;
  crc_indication_builder builder(msg);
  builder.set_pdu(to_rnti(10), to_harq_id(0), false, 1.235F, {}, {}, -13.125F);
  EXPECT_EQ(msg.pdu.rsrp, 1148U);
  EXPECT_EQ(msg.pdu.ul_sinr_metric, 617);
  builder.set_pdu(to_rnti(10), to_harq_id(0), false, -1.235F, {}, {}, -13.125F);
  EXPECT_EQ(msg.pdu.rsrp, 1148U);
  EXPECT_EQ(msg.pdu.ul_sinr_metric, -617);
}
