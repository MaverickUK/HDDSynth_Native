from hddsynth_companion import supercap


def test_levels():
    assert supercap.level(1100) == supercap.RED
    assert supercap.level(supercap.SPINDOWN_TYPICAL_MV - 1) == supercap.RED
    assert supercap.level(supercap.SPINDOWN_TYPICAL_MV) == supercap.ORANGE
    assert supercap.level(supercap.SPINDOWN_LONGEST_MV) == supercap.GREEN
    assert supercap.level(4150) == supercap.GREEN


def test_fraction_is_clamped_to_the_scale():
    assert supercap.fraction(0) == 0.0
    assert supercap.fraction(supercap.SCALE_MAX_MV + 500) == 1.0
    assert 0.5 < supercap.fraction(supercap.SPINDOWN_TYPICAL_MV) < 0.6
