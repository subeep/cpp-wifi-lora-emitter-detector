import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from tools.drone_ml.common import sha256, stamp
from tools.drone_ml.frequency_view_audit import (audit, centered_view, evaluate,
                                                overlap_fraction, reference_geometry)
from tools.drone_ml.preprocess import STFTConfig, spectrogram
from tools.drone_ml.recognition_review import (packet_entries, probe_block,
                                               validate_blocks, validate_frozen_review)


class RecognitionReviewTests(unittest.TestCase):
    def test_frozen_partitions_and_duplicate_members(self):
        row=dict(source='/original.mat',dataset='DRFF-R2',split_group='unit1',
                 label='unseen',partition='unknown_test_locked')
        review=dict(schema='rfmon.drone_ml.expanded_review.v1',records=[row])
        plan=dict(schema='rfmon.drone_ml.expanded_split.v1',groups={
            'unit1':dict(label='unseen',partition='unknown_test_locked')})
        validate_frozen_review(review,plan)
        plan['groups']['unit1']['partition']='train'
        with self.assertRaises(ValueError):validate_frozen_review(review,plan)
        plan['groups']['unit1']['partition']='unknown_test_locked'
        review['records'].append(row.copy())
        with self.assertRaises(ValueError):validate_frozen_review(review,plan)

    def test_report_exposure_never_becomes_truth(self):
        review={'records':[dict(dataset='DRFF-R2',source='/a/original.mat',label='unseen',
                                partition='unknown_test_locked',split_group='unit1')]}
        rows=[dict(dataset='DRFF-R2',drone='alias',source='original.mat',packet='P1'),
              dict(dataset='RFUAV',drone='unresolved',source='other.iq',packet='P1')]
        result=packet_entries(rows,review)
        self.assertTrue(result[0]['exposure_warning'])
        self.assertEqual(result[0]['existing_partitions'],['unknown_test_locked'])
        for r in result:
            self.assertFalse(r['training_eligible']);self.assertFalse(r['raw_iq_read_allowed'])
            self.assertIsNone(r['verified_presence_label']);self.assertIsNone(r['verified_identity_label'])
        self.assertEqual(result[1]['resolution'],'unresolved_do_not_infer')

    def test_sweep_bounds_overlap_and_nonfinite_frequency(self):
        a=dict(offset_bytes='0',num_samples='16',center_freq='100000000')
        b=dict(offset_bytes='128',num_samples='16',center_freq='100000000')
        self.assertEqual(validate_blocks([a,b],256),256)
        with self.assertRaises(ValueError):validate_blocks([a,b],255)
        with self.assertRaises(ValueError):validate_blocks([a,dict(b,offset_bytes='120')],256)
        with self.assertRaises(ValueError):validate_blocks([dict(a,center_freq='nan')],256)

    def test_zero_padding_probe_does_not_certify_continuity(self):
        x=np.r_[np.ones(131072,dtype='<c8'),np.zeros(131072,dtype='<c8')]
        p=probe_block(x.tobytes(),len(x),20e6)
        self.assertEqual(p['trailing_zero_samples'],131072)
        self.assertAlmostEqual(p['nonzero_extent_seconds'],.0065536)
        self.assertFalse(p['physical_continuity_verified']);self.assertFalse(p['training_eligible'])
        with self.assertRaises(ValueError):probe_block(x.tobytes()[:-1],len(x),20e6)

    def test_frequency_geometry_guard_and_invalid_reference(self):
        self.assertAlmostEqual(overlap_fraction(0,20,-15,5),.75)
        self.assertEqual(centered_view(-26e6,50e6,100e6),-20e6)
        with self.assertRaises(ValueError):centered_view(0,50e6,25e6)
        with self.assertRaises(ValueError):reference_geometry(dict(
            start_ms=0,duration_us=100,center_mhz=2440,bandwidth_mhz=float('nan')),
            20e6,2440e6,160000)
        with self.assertRaisesRegex(ValueError,'8 ms'):reference_geometry(dict(
            start_ms=0,duration_us=10000,center_mhz=2440,bandwidth_mhz=1),
            20e6,2440e6,400000)

    def test_production_parity_and_localization_changes_coverage(self):
        rate=100000000;n=round(rate*.008);t=np.arange(n)/rate
        # Two tones inside a broad reference region: five production centers
        # cannot contain both, whereas a sufficiently centered view can.
        x=(np.exp(2j*np.pi*(-34e6)*t)+.8*np.exp(2j*np.pi*(-19e6)*t)).astype('<c8')
        ref=dict(start_ms=1,duration_us=1000,center_mhz=2413.5,bandwidth_mhz=18.)
        r,arrays=evaluate(lambda s,n:x[s:s+n],n,rate,2440e6,ref)
        expected=spectrogram(x,rate,STFTConfig(duration_seconds=.008,hop=256,auto_frequency_view=True))
        np.testing.assert_array_equal(arrays['production_20'],expected)
        views={v['name']:v for v in r['views']}
        self.assertLess(views['production_20']['nominal_box_fraction'],1.)
        self.assertEqual(views['proposal_centered_25']['nominal_box_fraction'],1.)
        self.assertLess(views['production_20']['reference_region_native_power_fraction'],.999)
        self.assertAlmostEqual(views['proposal_centered_25']['reference_region_native_power_fraction'],1.)
        self.assertFalse(any(v['model_inference_performed'] for v in r['views']))

    def _fixture(self,root,partition='development_only'):
        meta=root/'meta';meta.mkdir();src=root/'sources';src.mkdir()
        rate=20000000;n=160000;t=np.arange(n)/rate
        x=(np.exp(2j*np.pi*1e6*t)+.02*np.random.default_rng(1).normal(size=n)).astype('<c8')
        path=src/'signal.cf32';x.tofile(path);digest=sha256(path)
        origin=dict(path=str(path),source='/archive.zip',archive_member='signal.mat',sha256=digest,
                    source_sample_offset=0,sample_rate_hz=rate,capture_center_hz=2440e6,partition=partition)
        row=dict(path=str(path),adapter='cf32_le',sample_rate_hz=rate,source_sha256=digest,
                 partition=partition,collection_group='same_unit')
        objects={'sources.json':dict(schema='rfmon.drone_ml.temporal_sources.v1',records=[row]),
                 'lineage.json':dict(records=[origin]),
                 'proposal.json':dict(source='signal.mat',sample_rate_msps=20.,center_freq_mhz=2440.,
                     packets=[dict(start_ms=1,duration_us=1000,center_mhz=2441.,bandwidth_mhz=2.)])}
        for name,obj in objects.items():(meta/name).write_text(json.dumps(obj))
        return meta,path

    def test_audit_preserves_iq_and_is_not_training_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);meta,path=self._fixture(root);before=stamp(path)
            r=audit(meta/'sources.json',meta/'lineage.json',[meta/'proposal.json'],root/'output')
            self.assertEqual(stamp(path),before);self.assertFalse(r['training_eligible'])
            self.assertFalse(r['final_test_iq_read']);self.assertFalse(r['model_inference_performed'])
            with self.assertRaises(FileExistsError):
                audit(meta/'sources.json',meta/'lineage.json',[meta/'proposal.json'],root/'output')

    def test_reserved_source_rejected_before_iq_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);meta,path=self._fixture(root,'unknown_test_locked');path.unlink()
            with self.assertRaisesRegex(ValueError,'reserved'):
                audit(meta/'sources.json',meta/'lineage.json',[meta/'proposal.json'],root/'output')
            self.assertFalse((root/'output').exists())


if __name__=='__main__':unittest.main()
