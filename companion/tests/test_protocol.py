"""The same sentences as the firmware's host tests (test/native/test_main.cpp), so the
app and the device agree byte for byte."""

from hddsynth_companion import protocol


def test_activity_known_answers():
    assert protocol.activity(False, "I", "-") == b"$HDA,1,0,I,-*28\r\n"
    assert protocol.activity(True, "S", "R") == b"$HDA,1,1,S,R*4C\r\n"
    assert protocol.activity(True, "R", "W") == b"$HDA,1,1,R,W*48\r\n"
    assert protocol.activity(True, "S", "W") == b"$HDA,1,1,S,W*49\r\n"


def test_commands_known_answers():
    assert protocol.query_status() == b"$HDQ,1,STATUS*78\r\n"
    assert protocol.query_packs() == b"$HDQ,1,PACKS*26\r\n"
    assert protocol.set_volume(55) == b"$HDC,1,VOL,55*07\r\n"
    assert protocol.set_balance(0) == b"$HDC,1,BAL,0*2D\r\n"
    assert protocol.set_balance(100) == b"$HDC,1,BAL,100*2C\r\n"
    assert protocol.set_pack(3) == b"$HDC,1,PACK,3*78\r\n"


def test_levels_are_clamped():
    assert protocol.set_volume(150) == protocol.set_volume(100)
    assert protocol.set_balance(-5) == protocol.set_balance(0)


def test_parse_status():
    r = protocol.parse_reply("$HDS,1,0.19.0,RUN,0,8,50,50,4150*27\r\n")
    assert r == protocol.Status("0.19.0", "RUN", 0, 8, 50, 50, 4150)


def test_parse_pack_and_ack():
    body = "HDP,1,2,8,PB_IBM_Ultrastar,IBM Ultrastar 36LZX"
    r = protocol.parse_reply(protocol.format_sentence(body).decode())
    assert r == protocol.PackEntry(2, 8, "PB_IBM_Ultrastar", "IBM Ultrastar 36LZX")
    ok = protocol.parse_reply(protocol.format_sentence("HDK,1,VOL,OK,").decode())
    assert ok == protocol.Ack("VOL", True, "")
    err = protocol.parse_reply(protocol.format_sentence("HDK,1,PACK,ERR,busy").decode())
    assert err == protocol.Ack("PACK", False, "busy")


def test_parse_ignores_everything_else():
    assert protocol.parse_reply("[stats] t=10s state=RUN heap=230504") is None
    assert protocol.parse_reply("$HDS,1,0.19.0,RUN,0,8,50,50,4150*28") is None  # checksum
    assert protocol.parse_reply("$HDS,2,0.19.0,RUN,0,8,50,50,4150*24") is None  # version 2
    assert protocol.parse_reply("$HDS,1,0.19.0,RUN,x,8,50,50,4150*6F") is None  # not a number
    assert protocol.parse_reply("$HDA,1,0,I,-*28") is None                      # not a reply
    assert protocol.parse_reply("") is None


def test_lower_case_checksum_accepted():
    assert protocol.parse_sentence("$HDA,1,1,S,R*4c") == ["HDA", "1", "1", "S", "R"]
