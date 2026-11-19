import unittest

import numpy as np

from tools.drone_ml.universal_c2_train import DUTY, IX, gate, model_probs, weights
from tools.drone_ml.universal_features import NAMES
from tools.drone_ml.universal_presence import K15, K30, KLB
from tools.drone_ml.universal_train import TYPES


def facts(**fired):
    v = np.zeros(len(NAMES), np.float32)
    for key, value in fired.items():
        v[DUTY if key == 'duty' else IX[{'k15': K15, 'k30': K30, 'klb': KLB}[key]]] = value
    return v


class GateTests(unittest.TestCase):
    models = ['Mini 2', 'Mini 4 Pro', 'Inspire 2', 'WLtoys']
    timing = {'Mini 2': K15, 'Mini 4 Pro': K30, 'Inspire 2': KLB, 'WLtoys': None}

    def allowed(self, **fired):
        t, m = gate(facts(**fired)[None], self.timing, self.models)
        return {TYPES[i] for i in np.flatnonzero(t[0])}, {self.models[i] for i in np.flatnonzero(m[0])}

    def test_nothing_measured_allows_only_not_a_drone(self):
        self.assertEqual(self.allowed()[0], {'not_a_drone'})

    def test_each_measurement_opens_its_own_type(self):
        self.assertEqual(self.allowed(k15=5)[0], {'dji_ofdm', 'not_a_drone'})
        self.assertEqual(self.allowed(klb=5)[0], {'lightbridge', 'not_a_drone'})
        self.assertEqual(self.allowed(duty=.1)[0], {'hopping_controller', 'not_a_drone'})
        self.assertEqual(self.allowed(k30=3.9)[0], {'not_a_drone'})       # threshold is > 4

    def test_dji_models_limited_to_the_timing_that_fired(self):
        self.assertEqual(self.allowed(k30=6)[1], {'Mini 4 Pro', 'Inspire 2', 'WLtoys'})
        self.assertEqual(self.allowed(k15=6, k30=6)[1], set(self.models))

    def test_model_probs_stay_inside_type_and_gate(self):
        model_type = [0, 0, 1, 2]
        allowed = np.array([[False, True, True, True]])
        p = model_probs(np.array([[9., 1., 5., 5.]]), 0, model_type, allowed)[0]
        self.assertEqual(p.tolist(), [0., 1., 0., 0.])

    def test_weights_balance_types_then_keys(self):
        meta = [dict(type='a', model='x')] * 3 + [dict(type='a', model='x', model_weight_key='x|mix')] + [dict(type='b', model='y')]
        w = weights(meta)
        self.assertAlmostEqual(w[:3].sum(), .25); self.assertAlmostEqual(w[3], .25); self.assertAlmostEqual(w[4], .5)


class AblationTests(unittest.TestCase):
    def test_no_facts_network_ignores_facts(self):
        import torch
        from tools.drone_ml.universal_c2_train import TwoLevel
        torch.manual_seed(0)
        net = TwoLevel(len(NAMES), 3, use_facts=False).eval()
        spec = torch.randn(2, 1, 128, 156)
        a = net(spec, torch.zeros(2, 2 * len(NAMES)), return_z=True)
        b = net(spec, torch.randn(2, 2 * len(NAMES)), return_z=True)
        for x, y in zip(a, b):
            self.assertTrue(torch.equal(x, y))


class RowRuleTests(unittest.TestCase):
    """C4: a row needs two typed captures of one type on one slot; names need distance and score."""

    def views(self, captures, model_logit=5.):
        from tools.drone_ml.universal_c4 import APP_VIEWS, Views
        n = len(captures) * 62
        v = Views.__new__(Views)
        v.gt = np.zeros((n, len(TYPES))); v.gt[:, TYPES.index('not_a_drone')] = 1
        v.meta = []
        for c, (minute, centre, typed) in enumerate(captures):
            for k in range(62):
                v.meta.append(dict(recording_id=f'cap{c}', view_index=k, minute=minute, centre_mhz=centre))
                if typed and k in APP_VIEWS:
                    v.gt[c * 62 + k] = 0; v.gt[c * 62 + k, TYPES.index('hopping_controller')] = 1
        v.allowed = v.gt > 0
        v.model_type = [TYPES.index('hopping_controller')] * 2
        v.ml = np.tile([model_logit, 0.], (n, 1)); v.am = np.ones((n, 2), bool); v.z = np.zeros((n, 96))
        v.mp = {t: model_probs(v.ml, t, v.model_type, v.am) for t in range(len(TYPES)) if t != TYPES.index('not_a_drone')}
        return v

    def rows(self, captures, distance=10., **kw):
        from tools.drone_ml.universal_c4 import Namer, background_rows
        namer = Namer(dict(means={'a': [0.] * 96, 'b': [1.] * 96}, precision=np.eye(96).tolist()), ['a', 'b'])
        th = {name: dict(threshold=.5) for name in TYPES}
        return background_rows(self.views(captures, **kw), th, dict(score_threshold=.9, distance_threshold=distance), namer)

    def test_two_typed_captures_on_one_slot_open_one_named_row(self):
        r = self.rows([(0, 2470, True), (1.1, 2470, True), (2.2, 2470, True), (30, 2410, False)])
        self.assertEqual((r['typed_captures'], r['rows'], r['named_rows']), (3, 1, 1))
        self.assertEqual(r['row_list'][0]['names'][0], ['a', 'b'])

    def test_isolated_or_spread_captures_open_no_row(self):
        r = self.rows([(0, 2470, True), (10, 2470, True), (1, 2450, True), (30, 2410, False)])
        self.assertEqual((r['typed_captures'], r['rows']), (3, 0))

    def test_name_refused_when_far_or_unsure(self):
        caps = [(0, 2470, True), (1.1, 2470, True)]
        self.assertEqual(self.rows(caps, distance=-1)['named_rows'], 0)          # too far from training views
        self.assertEqual(self.rows(caps, model_logit=.5)['named_rows'], 0)       # top score 0.62 < 0.9

    def test_per_type_limits_apply_to_their_own_type(self):
        from tools.drone_ml.universal_c4 import Namer, background_rows
        caps = [(0, 2470, True), (1.1, 2470, True)]
        namer = Namer(dict(means={'a': [0.] * 96, 'b': [1.] * 96}, precision=np.eye(96).tolist()), ['a', 'b'])
        th = {name: dict(threshold=.5) for name in TYPES}
        loose = dict(score_threshold=.9, distance_threshold=10.)
        for by_type, named in (({'hopping_controller': dict(loose, distance_threshold=-1), 'dji_ofdm': loose}, 0),
                               ({'hopping_controller': loose, 'dji_ofdm': dict(loose, distance_threshold=-1)}, 1),
                               ({'dji_ofdm': loose}, 0)):                       # no limits for the type: never named
            r = background_rows(self.views(caps), th, dict(by_type=by_type), namer)
            self.assertEqual(r['named_rows'], named)


if __name__ == '__main__':
    unittest.main()
