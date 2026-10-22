import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import h5py
import numpy as np

from tools.drone_ml.common import sha256, stamp
from tools.drone_ml.public_background import capture_layout, inventory, read_iq
from tools.drone_ml.temporal_audit import activity_summary, audit, check_record


class PublicTemporalTests(unittest.TestCase):
    def test_energy_islands_and_context_are_not_identity_labels(self):
        power=np.ones(1000)
        for start in range(10,1000,20):power[start:start+2]=100
        r=activity_summary(power,.001,6)
        self.assertEqual(r['energy_islands'],50)
        self.assertEqual(r['island_duration_ms_quantiles'],[2.,2.,2.,2.])
        contexts={x['requested_ms']:x for x in r['contexts']}
        self.assertLess(contexts[8]['fraction_containing_energy_activity'],.5)
        self.assertEqual(contexts[32]['fraction_containing_energy_activity'],1.)
        self.assertIn('not packet',r['interpretation'])

    def test_constant_traffic_and_short_context_do_not_manufacture_bursts(self):
        r=activity_summary(np.ones(10),.001,6)
        self.assertEqual(r['energy_islands'],0)
        self.assertIsNone(r['contexts'][-1]['fraction_containing_energy_activity'])

    def test_reserved_sources_rejected_before_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);m=root/'manifest.json'
            m.write_text(json.dumps(dict(schema='rfmon.drone_ml.temporal_sources.v1',records=[
                dict(partition='final_test',path='/sealed/not-readable') ])))
            with self.assertRaisesRegex(ValueError,'reserved'):
                audit(m,root/'report.json')
            self.assertFalse((root/'report.json').exists())

    def test_nist_layout_conversion_and_external_link_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'fixture.h5'
            with h5py.File(p,'w') as h:
                h['time']=np.array([32767,-32768,16384,0],dtype='<i2')
                self.assertEqual(capture_layout(h,expected_samples=2,expected_count=1),['time'])
                np.testing.assert_allclose(read_iq(h['time'],0,2),[32767/32768-1j,.5])
                with self.assertRaises(ValueError):read_iq(h['time'],1,2)
                h['external']=h5py.ExternalLink('/sealed/no.h5','x')
                with self.assertRaisesRegex(ValueError,'linked'):
                    capture_layout(h,expected_samples=2,expected_count=2)

    def test_incomplete_download_not_promoted(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sources=root/'originals';sources.mkdir()
            (sources/'2_4ghz_indoor.h5.part').write_bytes(b'incomplete')
            with self.assertRaisesRegex(ValueError,'incomplete'):
                inventory(sources,root/'inventory.json')
            self.assertFalse((sources/'2_4ghz_indoor.h5').exists())

    def test_verified_inventory_preserves_unknown_labels_and_group(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sources=root/'originals';sources.mkdir()
            part=sources/'2_4ghz_indoor.h5.part'
            with h5py.File(part,'w') as h:h['timestamp']=np.arange(4,dtype='<i2')
            (sources/'2_4ghz_indoor.csv').write_text('Property,Value,Units\nSampling Rate,30000000,Hz\nBitdepth,16,bits\nCenter Frequency,2437000000,Hz\n')
            spec=dict(bytes=part.stat().st_size,sha256=sha256(part),collection_group='one_collection',center_hz=2437000000)
            with patch('tools.drone_ml.public_background.METADATA_SHA256',sha256(sources/'2_4ghz_indoor.csv')),patch('tools.drone_ml.public_background.FILES',{'2_4ghz_indoor.h5':spec}),patch(
                    'tools.drone_ml.public_background.capture_layout',return_value=['timestamp']):
                r=inventory(sources,root/'inventory.json')
            self.assertTrue(r['publisher_sha256_verified'])
            row=r['records'][0]
            self.assertFalse(row['absence_verified']);self.assertFalse(row['training_eligible'])
            self.assertIsNone(row['verified_label']);self.assertEqual(row['collection_group'],'one_collection')
            self.assertFalse(part.exists())

    def test_native_iq_audit_preserves_sources_and_detects_tamper(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);sources=root/'originals';sources.mkdir();p=sources/'iq.cf32'
            rng=np.random.default_rng(8)
            x=(rng.normal(size=320000)+1j*rng.normal(size=320000)).astype('<c8')
            x.tofile(p)
            row=dict(path=str(p),adapter='cf32_le',sample_rate_hz=20000000,
                partition='development_only',collection_group='synthetic',source_sha256=sha256(p))
            m=root/'manifest.json';m.write_text(json.dumps(dict(schema='rfmon.drone_ml.temporal_sources.v1',records=[row])))
            before=stamp(p);r=audit(m,root/'report.json',.016)
            self.assertEqual(stamp(p),before)
            self.assertFalse(r['optimal_context_determined']);self.assertFalse(r['final_test_iq_read'])
            self.assertFalse(r['records'][0]['labels_created'])
            self.assertGreater(r['records'][0]['analysed_samples'],300000)
            x[0]=123;x.tofile(p)
            with self.assertRaisesRegex(ValueError,'SHA-256'):
                audit(m,root/'tampered.json',.016)


if __name__=='__main__':unittest.main()
