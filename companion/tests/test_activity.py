import pytest

from hddsynth_companion.activity import (
    HEARTBEAT_S,
    Activity,
    ActivityDetector,
    LinkSender,
    Sample,
    threshold_kb,
)

KB = 1024
TICK = 0.02


class Disk:
    """Cumulative counters that tests move forward one 20 ms tick at a time."""

    def __init__(self):
        self.t = 100.0
        self.rb = self.wb = self.rc = self.wc = 0

    def tick(self, read_kb=0, write_kb=0, ops=0):
        self.t += TICK
        self.rb += int(read_kb * KB)
        self.wb += int(write_kb * KB)
        if read_kb >= write_kb:
            self.rc += ops
        else:
            self.wc += ops
        return Sample(self.t, self.rb, self.wb, self.rc, self.wc)


def run(det, disk, ticks, **kw):
    a = None
    for _ in range(ticks):
        a = det.update(disk.tick(**kw))
    return a


def test_threshold_curve():
    assert threshold_kb(100) == pytest.approx(0)
    assert threshold_kb(60) == pytest.approx(35.7, abs=0.5)
    assert threshold_kb(0) == pytest.approx(8191)
    assert threshold_kb(150) == threshold_kb(100)  # clamped
    assert all(threshold_kb(s) > threshold_kb(s + 1) for s in range(100))


def test_quiet_disk_is_idle():
    det, disk = ActivityDetector(60), Disk()
    a = run(det, disk, 50)
    assert not a.busy and a.cls == "I" and a.op == "-"


def test_background_trickle_stays_below_threshold():
    # 4 KB every tick is 20 KB per 100 ms: under sensitivity 60's ~36 KB.
    det, disk = ActivityDetector(60), Disk()
    assert not run(det, disk, 50, read_kb=4, ops=1).busy
    det.sensitivity = 100  # but the most sensitive setting hears it
    assert run(det, disk, 1, read_kb=4, ops=1).busy


def test_random_reads():
    det, disk = ActivityDetector(60), Disk()
    a = run(det, disk, 10, read_kb=64, ops=8)  # 8 KB per op
    assert a.busy and a.cls == "R" and a.op == "R"


def test_sequential_copy_writes():
    det, disk = ActivityDetector(60), Disk()
    a = run(det, disk, 30, write_kb=4096, ops=4)  # 1 MB per op
    assert a.busy and a.cls == "S" and a.op == "W"


def test_activity_ends_after_the_window():
    det, disk = ActivityDetector(60), Disk()
    assert run(det, disk, 5, read_kb=512, ops=4).busy
    assert run(det, disk, 3).busy        # 60 ms later the burst is still in the 100 ms window
    assert not run(det, disk, 3).busy    # 120 ms later it isn't


def test_negative_deltas_are_ignored():
    det = ActivityDetector(100)
    det.update(Sample(1.0, 10 * KB * KB, 0, 1000, 0))
    a = det.update(Sample(1.02, 0, 0, 0, 0))  # a disk went away: counters dropped
    assert not a.busy


def test_link_sender_on_change_and_heartbeat():
    s = LinkSender()
    idle = Activity(False, "I", "-", 0, 36)
    busy = Activity(True, "R", "R", 100, 36)
    assert s.should_send(idle, 0.0)
    assert not s.should_send(idle, 0.1)
    assert s.should_send(busy, 0.12)        # changed
    assert not s.should_send(busy, 0.2)
    assert s.should_send(busy, 0.12 + HEARTBEAT_S)
    s.reset()
    assert s.should_send(busy, 0.33)        # after a reconnect, straight away
