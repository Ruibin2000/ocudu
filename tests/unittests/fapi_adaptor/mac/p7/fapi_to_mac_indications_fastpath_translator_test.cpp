// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "fapi_to_mac_indications_fastpath_translator.h"
#include "phy_to_fapi_results_event_fastpath_translator.h"
#include "ocudu/fapi/p7/messages/crc_indication.h"
#include <limits>
#include <optional>
#include "ocudu/fapi/p7/messages/rach_indication.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace fapi_adaptor;

namespace {
class mac_cell_rach_handler_dummy : public mac_cell_rach_handler
{
  mac_rach_indication indication;

public:
  void handle_rach_indication(const mac_rach_indication& rach_ind) override { indication = rach_ind; }

  const mac_rach_indication& get_indication() const { return indication; }
};
} // namespace

class mac_rach_indication_fixture : public testing::TestWithParam<float>
{
  mac_cell_rach_handler_dummy                 rach_handler;
  fapi_to_mac_indications_fastpath_translator translator;

protected:
  mac_rach_indication_fixture() : translator(0, ocudulog::fetch_basic_logger("FAPI"))
  {
    translator.set_cell_rach_handler(rach_handler);
  }

  void test_pdu()
  {
    const fapi::rach_indication& msg = build_message();
    translator.on_rach_indication(msg);
    check_pdu();
  }

  void test_power_values(float value)
  {
    rssi  = std::clamp(value, -140.F, 30.F);
    power = std::clamp(value, -140.F, 30.F);
    test_pdu();
  }

private:
  unsigned slot            = 0;
  unsigned sfn             = 1;
  float    rssi            = 14.F;
  unsigned slot_index      = 3;
  unsigned start_symbol    = 4;
  unsigned freq_index      = 5;
  float    snr             = 12.F;
  float    power           = 13.F;
  unsigned time_advance_ns = 8;

  fapi::rach_indication build_message()
  {
    fapi::rach_indication fapi_msg;
    auto                  scs = subcarrier_spacing::kHz30;
    fapi_msg.slot             = slot_point(scs, sfn, slot);

    fapi_msg.pdu.avg_rssi     = (rssi + 140) * 1000;
    fapi_msg.pdu.symbol_index = start_symbol;
    fapi_msg.pdu.slot_index   = slot_index;
    fapi_msg.pdu.ra_index     = freq_index;
    fapi_msg.pdu.preambles.emplace_back();
    fapi::rach_indication_pdu_preamble& preamble = fapi_msg.pdu.preambles.back();
    preamble.preamble_snr                        = (snr + 64) * 2;
    preamble.preamble_pwr                        = (power + 140) * 1000;
    preamble.timing_advance_offset               = phy_time_unit::from_seconds(time_advance_ns * 1e-9);

    return fapi_msg;
  }

  void check_pdu()
  {
    const mac_rach_indication&                msg = rach_handler.get_indication();
    const mac_rach_indication::rach_occasion& occ = msg.occasions.front();
    EXPECT_EQ(start_symbol, occ.start_symbol);
    EXPECT_EQ(slot_index, occ.slot_index);
    EXPECT_EQ(freq_index, occ.frequency_index);
    EXPECT_FLOAT_EQ(rssi, occ.rssi_dBFS.value());

    const mac_rach_indication::rach_preamble& pream = occ.preambles.front();
    EXPECT_FLOAT_EQ(power, pream.pwr_dBFS.value());
    EXPECT_EQ(time_advance_ns, std::round(pream.time_advance.to_seconds() * 1e9));
  }
};

TEST_P(mac_rach_indication_fixture, PowerRelatedValuesWorks)
{
  test_power_values(GetParam());
}

INSTANTIATE_TEST_SUITE_P(PowerValues, mac_rach_indication_fixture, testing::Values(-140.F, -20.F, 0.F, 30.F, 63.F));

TEST_F(mac_rach_indication_fixture, CorrectMessageConvertsCorrectly)
{
  test_pdu();
}

namespace {
class falcon_mac_crc_capture : public mac_cell_control_information_handler
{
public:
  std::optional<mac_crc_indication_message> last;
  void handle_crc(const mac_crc_indication_message& message) override { last = message; }
  void handle_uci(const mac_uci_indication_message&) override {}
  void handle_srs(const mac_srs_indication_message&) override {}
};

class falcon_fapi_crc_bridge : public fapi::p7_indications_notifier
{
public:
  explicit falcon_fapi_crc_bridge(fapi_to_mac_indications_fastpath_translator& mac_) : mac(mac_) {}
  std::optional<fapi::crc_indication> last;
  void on_crc_indication(const fapi::crc_indication& message) override
  {
    last = message;
    mac.on_crc_indication(message);
  }
  void on_rx_data_indication(const fapi::rx_data_indication&) override {}
  void on_uci_indication(const fapi::uci_indication&) override {}
  void on_srs_indication(const fapi::srs_indication&) override {}
  void on_rach_indication(const fapi::rach_indication&) override {}

private:
  fapi_to_mac_indications_fastpath_translator& mac;
};

class falcon_rf_chain_test : public ::testing::Test
{
protected:
  falcon_rf_chain_test() :
    mac(0, ocudulog::fetch_basic_logger("FAPI")),
    bridge(mac),
    phy(0, 1.0F, ocudulog::fetch_basic_logger("FAPI"))
  {
    mac.set_cell_crc_handler(capture);
    phy.set_p7_indications_notifier(bridge);
  }

  void send(std::optional<float> rsrp, std::optional<float> sinr, bool crc_ok = false)
  {
    // A full-scale amplitude reference of 1 gives a zero dBFS calibration offset.
    // Both translators below are production classes; this test does not clamp or
    // quantize the input itself.
    auto result = ul_pusch_results_data::create_discarded(to_rnti(0x4601), slot_point(0, 11), 3);
    result.csi = channel_state_information(channel_state_information::sinr_type::post_equalization);
    if (rsrp.has_value()) {
      result.csi.set_rsrp_dB(*rsrp);
    }
    if (sinr.has_value()) {
      result.csi.set_sinr_dB(channel_state_information::sinr_type::post_equalization, *sinr);
    }
    result.decoder_result.tb_crc_ok = crc_ok;
    bridge.last.reset();
    capture.last.reset();
    phy.on_new_pusch_results_data(result);
  }

  void expect_decoded(float rsrp, float sinr, uint16_t encoded_rsrp, int16_t encoded_sinr, bool crc_ok = false)
  {
    ASSERT_TRUE(bridge.last.has_value());
    ASSERT_TRUE(capture.last.has_value());
    ASSERT_EQ(capture.last->crcs.size(), 1U);
    const auto& pdu = capture.last->crcs[0];
    EXPECT_EQ(bridge.last->pdu.rsrp, encoded_rsrp);
    EXPECT_EQ(bridge.last->pdu.ul_sinr_metric, encoded_sinr);
    EXPECT_EQ(capture.last->sl_rx, slot_point(0, 11));
    EXPECT_EQ(pdu.rnti, to_rnti(0x4601));
    EXPECT_EQ(pdu.harq_id, 3U);
    EXPECT_EQ(pdu.tb_crc_success, crc_ok);
    ASSERT_TRUE(pdu.ul_rsrp_dBFS.has_value());
    ASSERT_TRUE(pdu.ul_sinr_dB.has_value());
    EXPECT_FLOAT_EQ(*pdu.ul_rsrp_dBFS, rsrp);
    EXPECT_FLOAT_EQ(*pdu.ul_sinr_dB, sinr);
  }

  falcon_mac_crc_capture capture;
  fapi_to_mac_indications_fastpath_translator mac;
  falcon_fapi_crc_bridge bridge;
  phy_to_fapi_results_event_fastpath_translator phy;
};

TEST_F(falcon_rf_chain_test, missing_phy_values_survive_fapi_sentinels_as_missing_mac_optionals)
{
  send({}, {});
  ASSERT_TRUE(bridge.last.has_value());
  ASSERT_TRUE(capture.last.has_value());
  EXPECT_EQ(bridge.last->pdu.rsrp, std::numeric_limits<uint16_t>::max());
  EXPECT_EQ(bridge.last->pdu.ul_sinr_metric, std::numeric_limits<int16_t>::min());
  ASSERT_EQ(capture.last->crcs.size(), 1U);
  EXPECT_FALSE(capture.last->crcs[0].ul_rsrp_dBFS.has_value());
  EXPECT_FALSE(capture.last->crcs[0].ul_sinr_dB.has_value());
}

TEST_F(falcon_rf_chain_test, measured_phy_zero_survives_both_translators_with_failed_crc)
{
  send(0.F, 0.F);
  expect_decoded(0.F, 0.F, 1280U, 0);
}

TEST_F(falcon_rf_chain_test, production_chain_quantizes_rsrp_and_positive_negative_sinr)
{
  send(-13.125F, 1.235F);
  expect_decoded(-13.2F, 1.234F, 1148U, 617);
  send(-13.125F, -1.235F, true);
  expect_decoded(-13.2F, -1.234F, 1148U, -617, true);
}

TEST_F(falcon_rf_chain_test, production_phy_clamps_positive_and_negative_saturation_before_quantization)
{
  // IEEE single-precision +/-65.534 is quantized by the production builder to
  // +/-32766 through float multiplication and truncation, then decoded by MAC.
  send(200.F, 200.F);
  expect_decoded(0.F, 65.532F, 1280U, 32766);
  send(-200.F, -200.F);
  expect_decoded(-128.F, -65.532F, 0U, -32766);
}
} // namespace
