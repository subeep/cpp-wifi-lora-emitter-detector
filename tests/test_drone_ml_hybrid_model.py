import unittest
import torch
from tools.drone_ml.hybrid_model import HybridDroneModel, FEATURES, region_loss, bag_loss, description


class HybridTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        torch.set_num_threads(2)

    def setUp(self):
        torch.manual_seed(7)
        self.model = HybridDroneModel(3).eval()
        self.args = [torch.randn(2, 16, 64), torch.rand(2, 16, len(FEATURES)),
                     torch.ones(2, 16, len(FEATURES), dtype=torch.bool),
                     torch.ones(2, 16, dtype=torch.bool), torch.zeros(2, 16, dtype=torch.bool)]

    def run_model(self, args):
        with torch.no_grad():
            return self.model.forward_features(*args)

    def test_no_future_leakage(self):
        a = self.run_model(self.args)
        changed = [x.clone() for x in self.args]
        changed[0][:, 8:] += 100
        changed[1][:, 8:] *= .1
        b = self.run_model(changed)
        prefix = self.run_model([x[:, :8] for x in self.args])
        for key in ('presence_logits', 'identity_logits', 'embedding'):
            torch.testing.assert_close(a[key][:, :8], b[key][:, :8], rtol=0, atol=0)
            torch.testing.assert_close(a[key][:, :8], prefix[key], rtol=1e-5, atol=1e-6)

    def test_explicit_retune_and_missing_tile_reset(self):
        for gap in (False, True):
            args = [x.clone() for x in self.args]
            args[4][:, 8] = True
            if gap:
                args[3][:, 7] = False
                args[0][:, 7] = float('nan')
                args[1][:, 7] = float('nan')
            actual = self.run_model(args)
            fresh = self.run_model([x[:, 8:] for x in args])
            for key in ('presence_logits', 'identity_logits', 'embedding'):
                torch.testing.assert_close(actual[key][:, 8:], fresh[key], rtol=1e-5, atol=1e-6)
                if gap:
                    self.assertEqual(float(actual[key][:, 7].abs().sum()), 0)

    def test_missing_measurements_never_become_values(self):
        self.args[2].zero_()
        a = self.run_model(self.args)
        self.args[1].fill_(float('nan'))
        b = self.run_model(self.args)
        for key in ('presence_logits', 'identity_logits'):
            torch.testing.assert_close(a[key], b[key], rtol=0, atol=0)
        self.args[2][0, 0, 0] = True
        with self.assertRaisesRegex(ValueError, 'DSP measurement'):
            self.run_model(self.args)

    def test_cnn_and_joint_loss_backward_without_invalid_labels(self):
        valid = torch.tensor([[True, False]])
        tiles = torch.randn(1, 2, 1, 256, 625); tiles[:, 1] = float('nan')
        out = self.model(tiles, torch.zeros(1, 2, len(FEATURES)),
                         torch.zeros(1, 2, len(FEATURES), dtype=torch.bool), valid,
                         torch.zeros(1, 2, dtype=torch.bool))
        loss = region_loss(out, torch.tensor([[1, -1]]), torch.tensor([[2, -1]]))
        loss.backward()
        self.assertTrue(torch.isfinite(loss))
        self.assertGreater(self.model.encoder[0].weight.grad.abs().sum(), 0)
        self.assertTrue(all(torch.isfinite(p.grad).all() for p in self.model.parameters() if p.grad is not None))
        with self.assertRaisesRegex(ValueError, 'invalid tiles'):
            region_loss(out, torch.tensor([[1, 0]]), torch.tensor([[2, -1]]))

    def test_weak_bags_never_supervise_absent_or_unknown_identity(self):
        p = torch.randn(3, 4, 2, requires_grad=True)
        i = torch.randn(3, 4, 3, requires_grad=True)
        out = dict(presence_logits=p, identity_logits=i, valid=torch.ones(3, 4, dtype=torch.bool),
                   segment_start=torch.zeros(3, 4, dtype=torch.bool))
        loss = bag_loss(out, torch.tensor([0, 1, 1]), torch.tensor([-1, -1, 2]))
        loss.backward()
        self.assertEqual(float(i.grad[:2].abs().sum()), 0)
        self.assertGreater(float(i.grad[2].abs().sum()), 0)
        out['segment_start'][0, 2] = True
        with self.assertRaisesRegex(ValueError, 'uninterrupted'):
            bag_loss(out, torch.tensor([0, 1, 1]), torch.tensor([-1, -1, 2]))
        with self.assertRaisesRegex(ValueError, 'positive bag'):
            bag_loss(out, torch.tensor([0, 1, 1]), torch.tensor([0, -1, 2]))

    def test_unqualified_and_bounded(self):
        self.assertFalse(description(3)['trained'])
        self.assertFalse(description(3)['live_qualified'])
        self.args[1][0, 0, 0] = 2
        with self.assertRaisesRegex(ValueError, '\\[0,1\\]'):
            self.run_model(self.args)


if __name__ == '__main__':
    unittest.main()
