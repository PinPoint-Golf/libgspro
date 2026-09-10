/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Mark Liversedge */
/*
 * test_decode.c — CT-D01 … CT-D24, docs/conformance.md §3.2.
 *
 * ⚠ EVERY CASE HERE IS A REAL CLIENT'S BYTES.  The tolerances this file pins —
 * case-insensitive keys, a float where an integer belongs, a null where an
 * object belongs, a zero standing in for a measurement — are not defensive
 * programming.  Each one is a launch monitor that is in use today and that
 * works against the real GSPro, so a decoder that refused it would lose shots
 * from a device somebody owns.  fixtures/README.md says which is which.
 */
#include "gspro/gspro.h"
#include "gs_test.h"

/* Decode a fixture, or fail the case and leave `out` zeroed. */
static void decode_fixture(const char *name, gsp_message *out)
{
    size_t len = 0;
    unsigned char *fx = gs_fixture(name, &len);
    gsp_status st;

    memset(out, 0, sizeof(*out));
    /* ⚠ The delimiter is NOT part of the object.  Three clients append a
     * newline; the decoder is handed exactly what the framer delimited. */
    while (len > 0 && (fx[len - 1] == '\n' || fx[len - 1] == '\r')) {
        len--;
    }
    st = gsp_message_decode(fx, len, out);
    GS_ASSERT_MSG(st == GSP_OK, name);
    free(fx);
}

static void decode_text(const char *json, gsp_message *out)
{
    memset(out, 0, sizeof(*out));
    GS_ASSERT_EQ(gsp_message_decode(GS_BYTES(json), out), GSP_OK);
}

/* CT-D01 — the vendor's own full example. [GSP] */
GS_TEST(CT_D01_vendor_full_example)
{
    gsp_message m;
    decode_fixture("gsp_full.json", &m);

    GS_ASSERT_STR(m.device_id, "GSPro LM 1.1");
    GS_ASSERT_EQ(m.units, GSP_UNITS_YARDS);
    GS_ASSERT_STR(m.units_text, "Yards");
    GS_ASSERT_EQ(m.shot_number, 13);
    GS_ASSERT_STR(m.api_version, "1");
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);

    GS_ASSERT_EQ(m.ball.present, (uint32_t)(GSP_BALL_SPEED | GSP_BALL_SPIN_AXIS |
                                            GSP_BALL_TOTAL_SPIN | GSP_BALL_BACK_SPIN |
                                            GSP_BALL_SIDE_SPIN | GSP_BALL_HLA |
                                            GSP_BALL_VLA | GSP_BALL_CARRY_DISTANCE));
    GS_ASSERT_NEAR(m.ball.speed, 147.5, 1e-9);
    GS_ASSERT_NEAR(m.ball.spin_axis, -13.2, 1e-9);
    GS_ASSERT_NEAR(m.ball.total_spin, 3250.0, 1e-9);
    GS_ASSERT_NEAR(m.ball.back_spin, 2500.0, 1e-9);
    GS_ASSERT_NEAR(m.ball.side_spin, -800.0, 1e-9);
    GS_ASSERT_NEAR(m.ball.hla, 2.3, 1e-9);
    GS_ASSERT_NEAR(m.ball.vla, 14.3, 1e-9);
    GS_ASSERT_NEAR(m.ball.carry_distance, 256.5, 1e-9);

    GS_ASSERT_EQ(m.club.present, 0x3ffu);   /* all ten, all zero */
    GS_ASSERT_EQ(m.options.contains_ball_data, 1);
    GS_ASSERT_EQ(m.options.contains_club_data, 0);
    GS_ASSERT_EQ(m.unknown_keys, 0);
    GS_ASSERT_MSG(m.flags == 0u, "the vendor's own example must decode with no findings");
    GS_ASSERT_MSG(m.ball.derived == 0u, "the decoder never derives");
}

/* CT-D02 — [TL] sends the five required ball keys and nothing else.  That is
 * COMPLETE by the vendor's rule: TotalSpin and SpinAxis are present, so the
 * BackSpin/SideSpin pair is not required. */
GS_TEST(CT_D02_five_required_keys_is_complete)
{
    gsp_message m;
    decode_fixture("tl_minimal.json", &m);

    GS_ASSERT_EQ(m.ball.present, (uint32_t)(GSP_BALL_SPEED | GSP_BALL_SPIN_AXIS |
                                            GSP_BALL_TOTAL_SPIN | GSP_BALL_HLA |
                                            GSP_BALL_VLA));
    GS_ASSERT_MSG((m.ball.present & (uint32_t)GSP_BALL_BACK_SPIN) == 0u,
                  "BackSpin was not sent and must not appear present");
    GS_ASSERT_MSG(gsp_message_ball_complete(&m.ball),
                  "TotalSpin + SpinAxis satisfies the vendor's rule");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_BALL_INCOMPLETE) == 0u,
                  "a five-key shot is not incomplete");
    GS_ASSERT_EQ(m.club.present, 0);
}

/* CT-D21 — and the SAME fixture carries no Units key at all, because [TL]
 * reads a field its constructor never set.  Absent Units is a real client. */
GS_TEST(CT_D21_absent_units_is_unknown_not_an_error)
{
    gsp_message m;
    decode_fixture("tl_minimal.json", &m);
    GS_ASSERT_EQ(m.units, GSP_UNITS_UNKNOWN);
    GS_ASSERT_STR(m.units_text, "");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_UNKNOWN_UNITS) == 0u,
                  "absent is not the same as unrecognised");
}

/* CT-D03 — [MLM] spells it "Backspin" and works against GSPro. */
GS_TEST(CT_D03_backspin_lower_case_s)
{
    gsp_message m;
    decode_fixture("mlm_backspin.json", &m);
    GS_ASSERT_MSG((m.ball.present & (uint32_t)GSP_BALL_BACK_SPIN) != 0u,
                  "'Backspin' must reach back_spin");
    GS_ASSERT_NEAR(m.ball.back_spin, 6184.1, 1e-9);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_KEY_CASE_MISMATCH) != 0u,
                  "accepted, and recorded as a variant spelling");
    GS_ASSERT_EQ(m.unknown_keys, 0);
}

/* CT-D04 — [R10] capitalises the V in APIversion; [SB] lower-cases the whole
 * tail.  Both must reach api_version. */
GS_TEST(CT_D04_api_version_case_variants)
{
    gsp_message m;
    decode_fixture("r10_apiversion.json", &m);
    GS_ASSERT_STR(m.api_version, "1");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_KEY_CASE_MISMATCH) != 0u,
                  "'APIVersion' is a variant of the documented 'APIversion'");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_MISSING_API_VERSION) == 0u,
                  "it was present, just spelled differently");

    decode_fixture("sb_strings.json", &m);
    GS_ASSERT_STR(m.api_version, "1");
}

/* CT-D07 — [R10] writes JSON nulls where an object is absent, and [OB] sends
 * "ClubData": null.  A null object is absence, not a malformed message. */
GS_TEST(CT_D07_null_object_is_absence)
{
    gsp_message m;

    decode_fixture("r10_apiversion.json", &m);
    GS_ASSERT_EQ(m.club.present, 0);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_CLUB_OBJECT_WITHOUT_FLAG) == 0u,
                  "a null ClubData is not an object");

    decode_fixture("ob_shot.json", &m);
    GS_ASSERT_EQ(m.club.present, 0);
    GS_ASSERT_EQ(m.options.contains_club_data, 0);
    GS_ASSERT_MSG(m.flags == (uint32_t)GSP_MSGF_UNKNOWN_KEYS,
                  "the only finding is the extra ClubName key");
}

/* And a null BOOLEAN in ShotDataOptions, which [R10] also emits. */
GS_TEST(CT_D07b_null_boolean_is_absence)
{
    gsp_message m;
    decode_fixture("r10_apiversion.json", &m);
    GS_ASSERT_EQ(m.options.contains_ball_data, 1);
    GS_ASSERT_MSG((m.options.present & (uint8_t)GSP_OPT_IS_HEARTBEAT) == 0u,
                  "IsHeartBeat: null was not sent");
    GS_ASSERT_EQ(m.options.is_heartbeat, 0);
}

/* CT-D08 — [OB] adds a top-level key the protocol does not define. */
GS_TEST(CT_D08_unknown_key_is_counted_not_fatal)
{
    gsp_message m;
    decode_fixture("ob_shot.json", &m);
    GS_ASSERT_EQ(m.unknown_keys, 1);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_UNKNOWN_KEYS) != 0u, "counted");
    GS_ASSERT_NEAR(m.ball.speed, 118.42, 1e-9);
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);
}

/* An unknown key whose value is a nested OBJECT or ARRAY must be skipped
 * whole — a decoder that resumed inside it would read its members as
 * top-level keys. */
GS_TEST(CT_D08b_unknown_nested_value_is_skipped_whole)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"Extra\":{\"Speed\":999,\"deep\":[1,{\"a\":2}]},"
                "\"ShotNumber\":4,"
                "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false}}",
                &m);
    GS_ASSERT_EQ(m.shot_number, 4);
    GS_ASSERT_EQ(m.unknown_keys, 1);
    GS_ASSERT_MSG(m.ball.present == 0u, "the Speed inside Extra is not ball data");
}

/* CT-D05 — [OSG] sends an integer as a float.  13.0 IS an integer. */
GS_TEST(CT_D05_float_shot_number_that_is_integral)
{
    gsp_message m;
    decode_fixture("osg_float_shot_number.json", &m);
    GS_ASSERT_EQ(m.shot_number, 13);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_SHOT_NUMBER_NOT_INTEGER) == 0u,
                  "13.0 is not a fractional shot number");
    /* Full-precision spins must survive as doubles. */
    GS_ASSERT_NEAR(m.ball.back_spin, 10454.780354572425, 1e-9);
}

/* CT-D06 — a genuinely fractional shot number is truncated and flagged. */
GS_TEST(CT_D06_fractional_shot_number_is_flagged)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"ShotNumber\":13.5,"
                "\"ShotDataOptions\":{\"ContainsBallData\":false,\"ContainsClubData\":false}}",
                &m);
    GS_ASSERT_EQ(m.shot_number, 13);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_SHOT_NUMBER_NOT_INTEGER) != 0u, "flagged");
}

/* CT-D09 — [SB] sends every value as a JSON string. */
GS_TEST(CT_D09_string_typed_values_are_coerced)
{
    gsp_message m;
    decode_fixture("sb_strings.json", &m);
    GS_ASSERT_NEAR(m.ball.speed, 147.5, 1e-9);
    GS_ASSERT_NEAR(m.ball.vla, 14.3, 1e-9);
    GS_ASSERT_EQ(m.options.contains_ball_data, 1);
    GS_ASSERT_EQ(m.options.contains_club_data, 0);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_TYPE_COERCED) != 0u,
                  "coerced, and said so");
}

/* CT-D10 — [OSP] and [OCR] send a ClubData holding only Speed. */
GS_TEST(CT_D10_club_data_speed_only)
{
    gsp_message m;

    decode_fixture("osp_shot.json", &m);
    GS_ASSERT_EQ(m.club.present, (uint32_t)GSP_CLUB_SPEED);
    GS_ASSERT_NEAR(m.club.speed, 101.86, 1e-9);
    GS_ASSERT_EQ(m.options.contains_club_data, 1);

    decode_fixture("ocr_putt.json", &m);
    GS_ASSERT_EQ(m.club.present, (uint32_t)GSP_CLUB_SPEED);
}

/* CT-D11 — [OF] measures two members and defaults the rest to a real 0.0, so
 * all ten are present and only the caller can say which mean anything. */
GS_TEST(CT_D11_club_data_partial_measurements)
{
    gsp_message m;

    decode_fixture("of_shot.json", &m);
    GS_ASSERT_EQ(m.club.present, 0x3ffu);
    GS_ASSERT_NEAR(m.club.speed, 110.0, 1e-9);
    GS_ASSERT_NEAR(m.club.path, 0.5, 1e-9);
    GS_ASSERT_NEAR(m.club.lie, 0.0, 1e-9);

    /* [GG] omits what it did not measure, so the mask is sparse. */
    decode_fixture("gg_shot_newline.json", &m);
    GS_ASSERT_EQ(m.club.present, (uint32_t)(GSP_CLUB_SPEED | GSP_CLUB_FACE_TO_TARGET |
                                            GSP_CLUB_LOFT | GSP_CLUB_VERTICAL_FACE_IMPACT |
                                            GSP_CLUB_CLOSURE_RATE));
    GS_ASSERT_MSG((m.club.present & (uint32_t)GSP_CLUB_PATH) == 0u,
                  "Path was omitted and must not read as measured zero");
}

/* CT-D12 — [SLX]: a shot whose ball speed is zero.  Zero is a VALUE: it is
 * delivered, present, unflagged, and a separate question answers it. */
GS_TEST(CT_D12_zero_speed_shot_is_a_shot)
{
    gsp_message m;
    decode_fixture("slx_zero_speed.json", &m);
    GS_ASSERT_EQ(m.kind, GSP_MSG_SHOT);
    GS_ASSERT_MSG((m.ball.present & (uint32_t)GSP_BALL_SPEED) != 0u, "present");
    GS_ASSERT_NEAR(m.ball.speed, 0.0, 1e-12);
    GS_ASSERT_MSG(gsp_message_ball_zero_speed(&m),
                  "the host needs to be able to ask this");

    decode_fixture("gsp_full.json", &m);
    GS_ASSERT_MSG(!gsp_message_ball_zero_speed(&m), "a real shot is not zero speed");
}

/* CT-D13 — [PIT] sends TotalSpin as a literal 0.0 placeholder beside a real
 * BackSpin/SideSpin pair.  ⚠ The decoder reports what arrived; derive() is what
 * repairs it, and says that it did. */
GS_TEST(CT_D13_pitrac_zero_total_spin_placeholder)
{
    gsp_message m;
    gsp_status st;
    decode_fixture("pit_shot.json", &m);

    GS_ASSERT_MSG((m.ball.present & (uint32_t)GSP_BALL_TOTAL_SPIN) != 0u,
                  "the key was on the wire, so it is present");
    GS_ASSERT_NEAR(m.ball.total_spin, 0.0, 1e-12);
    GS_ASSERT_MSG(m.ball.derived == 0u, "the decoder derives nothing");

    st = gsp_ball_data_derive(&m.ball);
    GS_ASSERT_EQ(st, GSP_OK);
    GS_ASSERT_NEAR(m.ball.total_spin, 5313.06, 0.05);   /* hypot(5300, 372.4) */
    GS_ASSERT_NEAR(m.ball.spin_axis, 4.019, 0.01);      /* atan2(372.4, 5300)  */
    GS_ASSERT_MSG((m.ball.derived & (uint32_t)GSP_BALL_TOTAL_SPIN) != 0u,
                  "a repaired total says it was repaired");
    GS_ASSERT_MSG((m.ball.derived & (uint32_t)GSP_BALL_BACK_SPIN) == 0u,
                  "what was measured is never marked derived");
}

/* CT-D14 — a consistent total and pair: derive() changes nothing. */
GS_TEST(CT_D14_consistent_spin_is_left_alone)
{
    gsp_message m;
    double total, axis;
    decode_fixture("gc2_shot.json", &m);
    total = m.ball.total_spin;
    axis = m.ball.spin_axis;

    GS_ASSERT_EQ(gsp_ball_data_derive(&m.ball), GSP_OK);
    GS_ASSERT_NEAR(m.ball.total_spin, total, 1e-12);
    GS_ASSERT_NEAR(m.ball.spin_axis, axis, 1e-12);
    GS_ASSERT_EQ(m.ball.derived, 0);
}

/* CT-D15 — the vendor's own example does NOT satisfy the identity: 3250 rpm
 * against a pair whose hypotenuse is 2625.  Report it; never "correct" it. */
GS_TEST(CT_D15_vendor_example_is_inconsistent_and_reported)
{
    gsp_message m;
    decode_fixture("gsp_full.json", &m);

    GS_ASSERT_EQ(gsp_ball_data_derive(&m.ball), GSP_PENDING);
    GS_ASSERT_NEAR(m.ball.total_spin, 3250.0, 1e-9);
    GS_ASSERT_NEAR(m.ball.spin_axis, -13.2, 1e-9);
    GS_ASSERT_MSG(m.ball.derived == 0u, "an inconsistency is reported, not resolved");
}

/* CT-D16 — the pair alone, and the zero-backspin edge both clients guard. */
GS_TEST(CT_D16_derive_pair_only_and_the_edge)
{
    gsp_ball_data b;

    memset(&b, 0, sizeof(b));
    b.back_spin = 2500.0;
    b.side_spin = -800.0;
    b.present = (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN);
    GS_ASSERT_EQ(gsp_ball_data_derive(&b), GSP_OK);
    GS_ASSERT_NEAR(b.total_spin, 2624.88, 0.01);
    GS_ASSERT_NEAR(b.spin_axis, -17.744, 0.01);
    GS_ASSERT_EQ(b.derived, (uint32_t)(GSP_BALL_TOTAL_SPIN | GSP_BALL_SPIN_AXIS));

    /* Backspin zero: ±90°, the case [GC2] special-cases to avoid a divide. */
    memset(&b, 0, sizeof(b));
    b.side_spin = 500.0;
    b.present = (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN);
    GS_ASSERT_EQ(gsp_ball_data_derive(&b), GSP_OK);
    GS_ASSERT_NEAR(b.total_spin, 500.0, 1e-9);
    GS_ASSERT_NEAR(b.spin_axis, 90.0, 1e-9);

    /* Total and axis alone: the pair comes back. */
    memset(&b, 0, sizeof(b));
    b.total_spin = 3000.0;
    b.spin_axis = 30.0;
    b.present = (uint32_t)(GSP_BALL_TOTAL_SPIN | GSP_BALL_SPIN_AXIS);
    GS_ASSERT_EQ(gsp_ball_data_derive(&b), GSP_OK);
    GS_ASSERT_NEAR(b.back_spin, 2598.076, 0.01);
    GS_ASSERT_NEAR(b.side_spin, 1500.0, 0.01);
    GS_ASSERT_EQ(b.derived, (uint32_t)(GSP_BALL_BACK_SPIN | GSP_BALL_SIDE_SPIN));

    /* Neither pair complete: a no-op, not an error. */
    memset(&b, 0, sizeof(b));
    b.speed = 100.0;
    b.present = (uint32_t)GSP_BALL_SPEED;
    GS_ASSERT_EQ(gsp_ball_data_derive(&b), GSP_OK);
    GS_ASSERT_EQ(b.present, (uint32_t)GSP_BALL_SPEED);
    GS_ASSERT_EQ(b.derived, 0);

    GS_ASSERT_EQ(gsp_ball_data_derive(NULL), GSP_ERR_INVALID_ARG);
}

/* CT-D17 — an over-long string truncates and says so. */
GS_TEST(CT_D17_over_long_device_id_truncates)
{
    gsp_message m;
    char json[512];
    char name[200];

    memset(name, 'A', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    snprintf(json, sizeof(json),
             "{\"DeviceID\":\"%s\",\"ShotDataOptions\":"
             "{\"ContainsBallData\":false,\"ContainsClubData\":false}}", name);

    decode_text(json, &m);
    GS_ASSERT_EQ(strlen(m.device_id), GSP_DEVICE_ID_MAX - 1);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_STRING_TRUNCATED) != 0u,
                  "truncation is never silent");
}

/* CT-D18 — [GC2] puts the unit's serial in DeviceID, which makes that field an
 * identifier: it is kept, and redacted when formatted for a log. */
GS_TEST(CT_D18_device_id_can_carry_a_serial)
{
    gsp_message m;
    char line[512];
    decode_fixture("gc2_shot.json", &m);

    GS_ASSERT_STR(m.device_id, "Foresight GC2 (2638)");

    GS_ASSERT(gsp_message_format(&m, line, sizeof(line), true) > 0);
    GS_ASSERT_CONTAINS(line, "2638");

    GS_ASSERT(gsp_message_format(&m, line, sizeof(line), false) > 0);
    GS_ASSERT_NOT_CONTAINS(line, "2638");
    GS_ASSERT_NOT_CONTAINS(line, "Foresight");
}

/* CT-D19 / CT-D20 — Units. */
GS_TEST(CT_D19_units_meters_is_carried_not_converted)
{
    gsp_message m;
    decode_text("{\"DeviceID\":\"x\",\"Units\":\"Meters\",\"BallData\":{\"Speed\":60.0},"
                "\"ShotDataOptions\":{\"ContainsBallData\":true,\"ContainsClubData\":false}}",
                &m);
    GS_ASSERT_EQ(m.units, GSP_UNITS_METERS);
    GS_ASSERT_STR(m.units_text, "Meters");
    GS_ASSERT_NEAR(m.ball.speed, 60.0, 1e-9);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_UNKNOWN_UNITS) == 0u, "Meters is known");

    /* Case-insensitive, like every other key and value we match. */
    decode_text("{\"Units\":\"yards\",\"ShotDataOptions\":"
                "{\"ContainsBallData\":false,\"ContainsClubData\":false}}", &m);
    GS_ASSERT_EQ(m.units, GSP_UNITS_YARDS);
}

GS_TEST(CT_D20_unrecognised_units_is_flagged_and_kept)
{
    gsp_message m;
    decode_text("{\"Units\":\"Metres\",\"ShotDataOptions\":"
                "{\"ContainsBallData\":false,\"ContainsClubData\":false}}", &m);
    GS_ASSERT_EQ(m.units, GSP_UNITS_UNKNOWN);
    GS_ASSERT_STR(m.units_text, "Metres");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_UNKNOWN_UNITS) != 0u,
                  "unrecognised is not the same as absent");
}

/* CT-D22 — a duplicated key: the last one wins, as every client's JSON
 * library does. */
GS_TEST(CT_D22_duplicate_key_last_wins)
{
    gsp_message m;
    decode_text("{\"BallData\":{\"Speed\":1.0,\"Speed\":2.0},"
                "\"ShotDataOptions\":{\"ContainsBallData\":true,\"ContainsClubData\":false}}",
                &m);
    GS_ASSERT_NEAR(m.ball.speed, 2.0, 1e-9);
}

/* CT-D23 — the number grammar strtod accepts, which is what arrives. */
GS_TEST(CT_D23_number_grammar)
{
    gsp_message m;
    decode_text("{\"BallData\":{\"Speed\":1.5e2,\"HLA\":-0.0,\"VLA\":+0,"
                "\"TotalSpin\":3.0E+3,\"SpinAxis\":-1e-2},"
                "\"ShotDataOptions\":{\"ContainsBallData\":true,\"ContainsClubData\":false}}",
                &m);
    GS_ASSERT_NEAR(m.ball.speed, 150.0, 1e-9);
    GS_ASSERT_NEAR(m.ball.hla, 0.0, 1e-12);
    GS_ASSERT_NEAR(m.ball.total_spin, 3000.0, 1e-9);
    GS_ASSERT_NEAR(m.ball.spin_axis, -0.01, 1e-12);
}

/* Missing root fields are delivered and flagged, never refused: "required" on
 * the vendor's page is what a client SHOULD send (design §4.5). */
GS_TEST(CT_D23b_missing_required_root_fields_are_flagged)
{
    gsp_message m;
    decode_text("{\"ShotDataOptions\":{\"ContainsBallData\":false,"
                "\"ContainsClubData\":false}}", &m);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_MISSING_DEVICE_ID) != 0u, "no DeviceID");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_MISSING_SHOT_NUMBER) != 0u, "no ShotNumber");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_MISSING_API_VERSION) != 0u, "no APIversion");
    GS_ASSERT_STR(m.device_id, "");

    /* A version that is present and not "1" is a different finding. */
    decode_text("{\"APIversion\":\"2\",\"ShotDataOptions\":"
                "{\"ContainsBallData\":false,\"ContainsClubData\":false}}", &m);
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_UNKNOWN_API_VERSION) != 0u, "not version 1");
    GS_ASSERT_MSG((m.flags & (uint32_t)GSP_MSGF_MISSING_API_VERSION) == 0u, "it was there");
}

/* CT-D24 — every truncation of every fixture.  ⚠ Worth running under
 * build/san: the port is open and unauthenticated, so the decoder is the one
 * component a hostile client can aim at (design §9.3). */
GS_TEST(CT_D24_every_truncation_of_every_fixture)
{
    static const char *const names[] = {
        "gsp_full.json", "tl_minimal.json", "mlm_backspin.json",
        "r10_apiversion.json", "r10_heartbeat.json", "osg_float_shot_number.json",
        "tnb_indented.json", "osp_connect_heartbeat.json", "osp_shot.json",
        "ocr_putt.json", "pit_shot.json", "pit_keepalive.json", "of_shot.json",
        "of_heartbeat.json", "ob_shot.json", "ob_heartbeat.json",
        "fb_shot_newline.json", "fb_heartbeat_newline.json", "gg_shot_newline.json",
        "gc2_shot.json", "sb_strings.json", "slx_zero_speed.json"
    };
    size_t i;
    int decoded_something = 0;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t len = 0, n;
        unsigned char *fx = gs_fixture(names[i], &len);
        for (n = 0; n <= len; ++n) {
            gsp_message m;
            gsp_status st;
            memset(&m, 0xa5, sizeof(m));
            st = gsp_message_decode(fx, n, &m);
            /* The only requirement is that it returns, does not read past `n`,
             * and never claims success on something that is not an object. */
            if (st == GSP_OK) {
                decoded_something = 1;
                GS_ASSERT_MSG(n > 0 && (n == len || fx[n - 1] == '}'),
                              "success on a truncation that is not a whole object");
            }
        }
        free(fx);
    }
    GS_ASSERT_MSG(decoded_something, "the sweep never once succeeded — is decode wired up?");
}

GS_TEST_MAIN()
