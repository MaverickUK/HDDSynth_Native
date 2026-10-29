from types import SimpleNamespace

from hddsynth_companion import device


def port(dev, vid, product=None, manufacturer=None, description=None):
    return SimpleNamespace(device=dev, vid=vid, product=product, manufacturer=manufacturer, description=description)


def test_finds_the_hdd_synth():
    ports = [
        port("/dev/cu.Bluetooth-Incoming-Port", None),
        port("/dev/cu.usbmodem2101", 0x2E8A, "Pico", "Raspberry Pi"),  # some other Pico
        port("/dev/cu.usbmodem1101", 0x2E8A, "HDDSYNTH", "HDD Synth"),
    ]
    assert device.find_port(ports) == "/dev/cu.usbmodem1101"


def test_windows_description_only():
    # Windows often reports only a description such as "HDDSYNTH (COM5)".
    assert device.find_port([port("COM5", 0x2E8A, description="HDDSYNTH (COM5)")]) == "COM5"


def test_none_found():
    assert device.find_port([port("COM3", 0x0403, "FT232R USB UART")]) is None
    assert device.find_port([]) is None


def test_candidates_put_named_ports_first():
    ports = [
        port("COM3", 0x2E8A, description="USB Serial Device (COM3)"),  # Windows hides the name
        port("COM4", 0x0403, "FT232R USB UART"),
        port("COM5", 0x2E8A, "HDDSYNTH", "HDD Synth"),
    ]
    assert device.candidate_ports(ports) == ["COM5", "COM3"]


def test_unnamed_pico_is_only_a_candidate():
    ports = [port("COM3", 0x2E8A, description="USB Serial Device (COM3)")]
    assert device.find_port(ports) is None
    assert device.candidate_ports(ports) == ["COM3"]
