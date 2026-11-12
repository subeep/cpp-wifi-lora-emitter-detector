import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from tools.drone_ml.x310_session import check, collection, template

RATE = 20_000_000


def make_session(root, name, start='2026-11-12T10:00:00+05:30', samples=4000, overflow=0, zero=False, centres=(2401e6, 5746.5e6)):
    d = Path(root) / name
    d.mkdir()
    session = dict(schema=1, target='DJI Air 3S', label=name, created=start, rate_hz=RATE, gain_db=20.0,
                   antenna='RX2', device_args='addr=192.168.10.2', format='sc16_le (int16 I, int16 Q interleaved)',
                   notes='', receive_only=True, segments=[])
    rng = np.random.default_rng(1)
    for i, c in enumerate(centres):
        raw = (np.zeros(samples * 2) if zero else rng.normal(0, 300, samples * 2)).astype('<i2').tobytes()
        f = d / f'seg{i:03d}_{c / 1e6:.0f}MHz.sc16'
        f.write_bytes(raw)
        seg = dict(file=f.name, centre_hz=c, samples=samples, requested_samples=samples, bytes=len(raw),
                   overflow_marks=overflow, status='ok' if not overflow else 'overflow',
                   host_start=start[:17] + f'{i * 5:02d}' + start[19:], sha256=hashlib.sha256(raw).hexdigest())
        session['segments'].append(seg)
        f.with_suffix('.json').write_text(json.dumps(seg))
    (d / 'session.json').write_text(json.dumps(session))
    return d


def scene_for(d, kind='quiet', partition='train', group='g1', day='2026-11-12', env='lab-a', state='off', approve=True):
    s = template(d)
    s.update(scene_type=kind, operator='Sudeep', environment_id=env, collection_day=day, collection_group=group, partition=partition,
             receiver=dict(antenna_description='dual-band whip', antenna_position='bench, 1 m', cable_or_filter='none'),
             own_drone_systems=[dict(model='DJI Air 3S', physical_unit='air3s-A', aircraft_state=state,
                                     controller_state=state, distance_m=3)],
             uncontrolled_drone_activity=dict(assessment='none_observed', basis='indoor lab, own aircraft battery removed'),
             nearby_equipment=[dict(kind='router', description='lab AP', state='idle', band='2.4/5')],
             timeline=[dict(time='2026-11-12T10:00:30+05:30', note='scene confirmed')],
             approval=dict(approved_by='Sudeep', approved_at='2026-11-12T11:00:00+05:30', statement='controlled scene') if approve
             else dict(approved_by=None, approved_at=None, statement=None))
    (d / 'scene.json').write_text(json.dumps(s))
    return s


class X310SessionTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def test_approved_background_is_only_a_candidate(self):
        d = make_session(self.tmp.name, 's1'); scene_for(d)
        r = check(d, verify_hashes=True)
        self.assertTrue(r['passed'], r['blockers'])
        self.assertEqual(r['label_candidate'], 'scoped_negative_candidate')
        self.assertFalse(r['label_assigned'])
        self.assertAlmostEqual(r['iq_seconds'], 2 * 4000 / RATE)

    def test_missing_or_unapproved_scene_never_becomes_negative(self):
        d = make_session(self.tmp.name, 's1')
        self.assertIn('scene.json missing', check(d)['blockers'])
        scene_for(d, approve=False)
        r = check(d)
        self.assertTrue(r['passed'])
        self.assertEqual(r['label_candidate'], 'unlabelled_stress')

    def test_powered_on_system_blocks_background_and_possible_activity_blocks_negative(self):
        d = make_session(self.tmp.name, 's1'); scene_for(d, state='on_linked')
        self.assertFalse(check(d)['passed'])
        s = scene_for(d); s['uncontrolled_drone_activity'] = dict(assessment='possible', basis='neighbour flies')
        (d / 'scene.json').write_text(json.dumps(s))
        self.assertEqual(check(d)['label_candidate'], 'unlabelled_stress')

    def test_recorder_faults_block(self):
        for kwargs, text in ((dict(overflow=2), 'overflow'), (dict(zero=True), 'all-zero')):
            d = make_session(self.tmp.name, 'bad' + text, **kwargs); scene_for(d)
            r = check(d)
            self.assertFalse(r['passed'])
            self.assertTrue(any(text in w for w in r['warnings']), r['warnings'])
            self.assertEqual(r['iq_seconds'], 0)
        d = make_session(self.tmp.name, 'trunc'); scene_for(d)
        seg = d / 'seg000_2401MHz.sc16'; seg.write_bytes(seg.read_bytes()[:-8])
        self.assertFalse(check(d)['passed'])
        d = make_session(self.tmp.name, 'tamper'); scene_for(d)
        seg = d / 'seg000_2401MHz.sc16'; raw = bytearray(seg.read_bytes()); raw[0] ^= 1; seg.write_bytes(raw)
        self.assertTrue(check(d)['passed'])
        self.assertFalse(check(d, verify_hashes=True)['passed'])

    def test_few_faulty_segments_are_excluded_not_repaired(self):
        d = make_session(self.tmp.name, 'long', centres=tuple(2401e6 + i * 1e6 for i in range(12))); scene_for(d)
        seg = d / 'seg003_2404MHz.sc16'; seg.write_bytes(seg.read_bytes()[:-8])
        r = check(d)
        self.assertTrue(r['passed'], r['blockers'])
        self.assertEqual((r['segments_usable'], r['segments_excluded']), (11, 1))
        self.assertAlmostEqual(r['iq_seconds'], 11 * 4000 / RATE)
        self.assertNotIn(2404e6, [s['centre_hz'] for s in r['segments'] if s['usable']])

    def test_sealed_final_iq_is_not_opened(self):
        d = make_session(self.tmp.name, 'final', zero=True); scene_for(d, partition='final_sealed')
        r = check(d, verify_hashes=True)
        self.assertTrue(r['passed'], r['blockers'])       # zero content is not inspected
        self.assertIs(r['sealed_iq_opened'], False)
        self.assertFalse(r['hashes_verified'])

    def test_controller_only_and_positive_candidates(self):
        d = make_session(self.tmp.name, 'rc'); s = scene_for(d, kind='controller_only')
        s['own_drone_systems'][0].update(aircraft_state='off', controller_state='on_unlinked')
        (d / 'scene.json').write_text(json.dumps(s))
        self.assertEqual(check(d)['label_candidate'], 'controller_only_activity_candidate')
        s['own_drone_systems'][0]['aircraft_state'] = 'on_linked'
        (d / 'scene.json').write_text(json.dumps(s))
        self.assertFalse(check(d)['passed'])

    def test_collection_rejects_shared_day_across_partitions_and_unpaired_settings(self):
        a = make_session(self.tmp.name, 'a'); scene_for(a, group='g1')
        b = make_session(self.tmp.name, 'b'); scene_for(b, group='g2', partition='development', env='lab-b')
        r = collection([check(a), check(b)])
        self.assertTrue(any('collection_day' in x for x in r['blockers']))
        self.assertTrue(r['unpaired_receiver_settings'])
        c = make_session(self.tmp.name, 'c'); scene_for(c, kind='linked_ground', state='on_linked', group='g1')
        r = collection([check(a), check(c)])
        self.assertTrue(r['passed'], r['blockers'])
        self.assertEqual(r['exposure']['train/scoped_negative_candidate']['sessions'], 1)


if __name__ == '__main__':
    unittest.main()
