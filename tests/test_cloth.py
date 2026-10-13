import numpy as np

from carpeted.cloth import Cloth, _slice_pairs


def edges_of(rows, cols, di, dj, axis, stride):
    idx = np.arange(rows * cols).reshape(rows, cols)
    groups = []
    for a, b in _slice_pairs(rows, cols, di, dj, axis, stride):
        groups.append(list(zip(idx[a].ravel(), idx[b].ravel())))
    return groups


def test_slice_groups_cover_every_edge_once_without_shared_particles():
    rows, cols = 7, 9
    for di, dj, axis, stride in [(0, 1, 1, 2), (1, 0, 0, 2), (0, 2, 1, 4),
                                 (2, 0, 0, 4), (1, 1, 0, 2), (1, -1, 0, 2)]:
        groups = edges_of(rows, cols, di, dj, axis, stride)
        expected = {(i * cols + j, (i + di) * cols + j + dj)
                    for i in range(rows) for j in range(cols)
                    if 0 <= i + di < rows and 0 <= j + dj < cols}
        got = [e for g in groups for e in g]
        assert len(got) == len(expected) and set(got) == expected
        for g in groups:
            used = [v for e in g for v in e]
            assert len(used) == len(set(used))


def test_flat_rug_stays_put_and_falls_asleep():
    c = Cloth(400, 250, 21, 13, (50, 50))
    start = c.pos.copy()
    for _ in range(80):
        c.step(1 / 60)
    assert np.abs(c.pos - start).max() < 1e-6
    assert c.sleeping


def test_folding_a_corner_turns_it_over_and_it_stays_folded():
    c = Cloth(400, 250, 21, 13, (50, 50))
    corner = c.idx(0, c.cols - 1)
    for k in range(90):
        c.pin(corner, (450 - k * 3.5, 50 + k * 2.0, 50))
        c.step(1 / 60)
    c.unpin_all()
    for _ in range(240):
        c.step(1 / 60)
    assert (c.normals[:, 2] < 0).sum() > 10          # back side is up
    assert c.pos[corner, 0] < 300                    # corner stayed folded over
    P = c.pos.reshape(c.rows, c.cols, 3)
    stretch = np.linalg.norm(np.diff(P, axis=1), axis=2) / c.dx
    assert stretch.max() < 1.1


def test_obstacles_make_a_bump():
    c = Cloth(400, 250, 21, 13, (50, 50))
    c.set_obstacles([(230, 150, 270, 190)])
    for _ in range(120):
        c.step(1 / 60)
    assert 8 < c.pos[:, 2].max() <= 14.01
    assert not c.sleeping or c.pos[:, 2].max() > 0


def test_state_roundtrip_and_validation():
    c = Cloth(400, 250, 21, 13, (50, 50))
    c.pos[:, 2] = 3.0
    state = c.state()
    d = Cloth(400, 250, 21, 13, (0, 0))
    assert d.load_state(state)
    assert np.allclose(d.pos, c.pos, atol=0.01)   # saved with 2 decimals
    assert not Cloth(500, 250, 21, 13).load_state(state)
    bad = dict(state, pos=[[0, 0]])
    assert not d.load_state(bad)
