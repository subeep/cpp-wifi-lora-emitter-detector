import unittest
from tools.drone_ml.universal_splits import check, partition, targets, OPEN_MODELS


def row(model, dataset='DRFF-R2', aircraft=None, source='/x/a.mat', **kw):
    return dict(model=model, dataset=dataset, aircraft=aircraft, source=source, member=None, recording_id='r', signal_type='dji_ofdm', **kw)


class SplitTests(unittest.TestCase):
    def test_aircraft_decides_multi_aircraft_partitions(self):
        multi = {'Air 2', 'DroneRFb A (code 84)'}
        self.assertEqual(partition(row('Air 2', aircraft='DRFF-R2:mavicAir2_1'), multi), 'train')
        self.assertEqual(partition(row('Air 2', aircraft='DRFF-R2:mavicAir2_3'), multi), 'val')
        self.assertEqual(partition(row('Air 2', aircraft='DRFF-R2:mavicAir2_4'), multi), 'test')
        self.assertEqual(partition(row('DroneRFb A (code 84)', 'DroneRFb-DIR', 'DroneRFb-DIR:A3'), multi), 'test')
        self.assertIn(partition(row('DroneRFb A (code 84)', 'DroneRFb-DIR', 'DroneRFb-DIR:A1'), multi), ('train', 'val'))

    def test_open_cross_and_background(self):
        for model in OPEN_MODELS:
            self.assertEqual(partition(row(model), set()), 'open_test')
        self.assertEqual(partition(row('Mini 4 Pro', 'RFUAV'), {'Mini 4 Pro'}), 'cross_test')
        self.assertEqual(partition(row('not_a_drone', 'Own_X310', background_partition='held_out'), set()), 'bg_test')
        self.assertIn(partition(row('not_a_drone', 'Own_X310', background_partition='train'), set()), ('train', 'val'))

    def test_only_measured_windows_become_targets(self):
        labels = [dict(state='signal_present'), dict(state='unknown'), dict(state='background')]
        self.assertEqual(targets(row('Mini 2'), labels, {}), [('dji_ofdm', 'Mini 2'), None, ('not_a_drone', 'x310_background')])
        overlay = {'r': dict(states=['signal_present', 'source_noise', 'unknown'])}
        self.assertEqual(targets(row('WLtoys', 'KULeuven_DroneRF'), labels, overlay),
                         [('hopping_controller', 'WLtoys'), ('not_a_drone', 'chamber_noise'), None])
        hopper = dict(row('FUTABA T14SG', 'RFUAV'), signal_type='hopping_controller')
        self.assertEqual(targets(hopper, labels[:1], {}), [None])     # RFUAV controllers stay unused

    def test_dronerfa_split_by_flight_and_segment(self):
        rfa = lambda model, distance, segment, **kw: partition(
            row(model, 'DroneRFa', 'DroneRFa:' + model, source='/d/DroneRFa.rar', distance=distance, segment=segment, **kw), {'Air 2S'})
        self.assertEqual([rfa('Phantom 4 Pro', d, 'S0000') for d in ('D00', 'D01', 'D10')], ['train', 'val', 'unit_test'])
        self.assertEqual(rfa('Air 2S', 'D10', 'S1000'), 'unit_test')        # one DroneRFa unit, not an unseen-aircraft test
        self.assertEqual([rfa('Mavic 3', None, s) for s in ('S0000', 'S0101', 'S0110', 'S1001', 'S1011')],
                         ['train', 'val', 'unit_test', 'train', 'unit_test'])
        self.assertEqual(rfa('Mini 3 Pro', 'D00', 'S0000'), 'open_test')
        self.assertEqual(rfa('Inspire 2', None, 'S0000'), 'train')          # trained on KU Leuven and DroneRFa
        self.assertEqual(partition(row('Inspire 2', 'Tampere_DroneRF'), set()), 'cross_test')
        self.assertEqual(partition(row('Phantom 4 Pro', 'Own_X310'), set()), 'cross_test')   # the user's own aircraft
        self.assertEqual([rfa('not_a_drone', 'D00', s) for s in ('S0000', 'S0111')], ['train', 'val'])
        self.assertEqual(targets(row('not_a_drone', 'DroneRFa'), [dict(state='background')], {}), [('not_a_drone', 'dronerfa_background')])

    def test_air3s_split_by_session(self):
        own = lambda src: partition(row('Air 3S', 'Own_X310', 'Own_X310:Air_3S:unit_1', source=src), set())
        self.assertEqual(own('/h/air3s_whole/air3s10/air3s/iq.bin'), 'train')
        self.assertEqual(own('/h/air3s_whole/air3s20/air3s/iq.bin'), 'val')
        self.assertEqual(own('/d/air3s_o4/2026-10-23/connected-live-feed-01/2-2452.cf32'), 'unit_test')
        self.assertNotIn('Air 3S', OPEN_MODELS)
        with self.assertRaises(ValueError):
            own('/elsewhere/new.bin')

    def test_no_presence_filter_labels_every_drone_window(self):
        labels = [dict(state='signal_present'), dict(state='unknown'), dict(state='background')]
        self.assertEqual(targets(row('Mini 2'), labels, {}, presence_filter=False), [('dji_ofdm', 'Mini 2')] * 3)
        overlay = {'r': dict(states=['signal_present', 'source_noise', 'unknown'])}
        self.assertEqual(targets(row('WLtoys', 'KULeuven_DroneRF'), labels, overlay, presence_filter=False),
                         [('hopping_controller', 'WLtoys')] * 3)
        background = row('not_a_drone', 'Own_X310')
        self.assertEqual(targets(background, labels, {}, False), targets(background, labels, {}))

    def test_leak_is_refused(self):
        groups = {'Air 2': {'train': {'u1'}, 'test': {'u1'}}}
        with self.assertRaises(ValueError):
            check([], groups, {'Air 2'})
        with self.assertRaises(ValueError):
            check([dict(recording_id='a', partition='train', model='Mini 3 Pro')], {}, set())


if __name__ == '__main__':
    unittest.main()
