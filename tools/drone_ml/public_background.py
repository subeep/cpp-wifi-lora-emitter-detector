"""Verify and inventory public NIST IQ without assigning verified absence labels.

Download separately to *.part. Promotion to a source filename requires the
publisher hash, byte count and layout. This inventory is NOT a training corpus.
"""
import argparse
import csv
import json
import os
from pathlib import Path

import h5py
import numpy as np

from .common import read_source, sha256, stamp
from .live_audit import write_new_json

DOI = 'https://doi.org/10.18434/mds2-2731'
BASE = 'https://data.nist.gov/od/ds/ark:/88434/mds2-2731/'
METADATA_SHA256 = '58d802790b01e6cb5dd7c2bb241cf41c4e8c08610931c77ce23d9b238e7cfe4f'
FILES = {
    '2_4ghz_indoor.h5': dict(bytes=21600072448,
        sha256='73948e960d0c222c685875cbbeb1556dce1ab04dd500b3cc00dbfb46253b14f3',
        collection_group='nist_2021_11_simultaneous_2_4ghz_wifi', center_hz=2437000000),
}


def metadata(path):
    with read_source(path) as raw:
        rows=list(csv.DictReader(raw.read().decode('utf-8-sig').splitlines()))
    values={r['Property']:r['Value'] for r in rows}
    if (float(values['Sampling Rate'])!=30000000 or int(values['Bitdepth'])!=16
            or float(values['Center Frequency'])!=2437000000):
        raise ValueError('unexpected NIST acquisition metadata')
    return values


def capture_layout(h, expected_samples=30000000, expected_count=180):
    keys=sorted(h.keys())
    if len(keys)!=expected_count:
        raise ValueError('unexpected NIST capture count')
    for key in keys:
        # Do not follow external or symbolic links from an untrusted HDF5 file.
        if not isinstance(h.get(key,getlink=True),h5py.HardLink):
            raise ValueError('linked dataset forbidden')
        ds=h[key]
        if (not isinstance(ds,h5py.Dataset) or ds.is_virtual or ds.external
                or ds.dtype.kind!='i' or ds.dtype.itemsize!=2
                or ds.shape!=(expected_samples*2,)):
            raise ValueError('unexpected interleaved int16 NIST layout')
    return keys


def read_iq(ds, offset, count):
    if (type(offset) is not int or type(count) is not int or offset<0
            or not 0<count<=4000000 or (offset+count)*2>ds.size):
        raise ValueError('invalid bounded IQ read')
    raw=np.asarray(ds[offset*2:(offset+count)*2],dtype=np.float32)/32768.
    return (raw[0::2]+1j*raw[1::2]).astype(np.complex64)


def inventory(root, output):
    root=Path(root).resolve();output=Path(output).resolve()
    if output==root or root in output.parents:
        raise ValueError('inventory output must be outside downloaded originals')
    if output.exists():raise FileExistsError(output)
    name='2_4ghz_indoor.h5';spec=FILES[name]
    target=root/name
    source=target if target.exists() else root/(name+'.part')
    before=stamp(source)
    if before[2]!=spec['bytes']:raise ValueError('download incomplete or wrong byte count')
    if sha256(source)!=spec['sha256']:raise ValueError('publisher SHA-256 mismatch')
    csv_path=root/'2_4ghz_indoor.csv'
    if sha256(csv_path)!=METADATA_SHA256:raise ValueError('publisher metadata SHA-256 mismatch')
    csv_before=stamp(csv_path);meta=metadata(csv_path)
    with read_source(source) as raw,h5py.File(raw,'r') as h:
        keys=capture_layout(h)
    if stamp(source)!=before or stamp(csv_path)!=csv_before:
        raise ValueError('sources changed during verification')
    if source!=target:
        # No existing complete source is overwritten.
        os.link(source,target)
        source.unlink()
    rows=[dict(path=str(target),adapter='nist_i16_hdf5',dataset_key=k,
        sample_rate_hz=30000000,center_hz=spec['center_hz'],samples=30000000,
        duration_seconds=1.,source_sha256=spec['sha256'],
        collection_group=spec['collection_group'],
        pair_group=f'nist_2021_11_2_4ghz:{k}',partition='development_only',
        publisher_context='ordinary Wi-Fi collection; uncontrolled nearby emitters',
        verified_label=None,training_eligible=False,absence_verified=False,
        continuity='publisher one-second capture; cross-capture continuity not established') for k in keys]
    report=dict(schema='rfmon.drone_ml.public_background_inventory.v1',source_doi=DOI,
        source_url=BASE+name,publisher_sha256_verified=True,byte_count=spec['bytes'],
        metadata=meta,metadata_sha256=sha256(csv_path),records=rows,
        live_qualified=False,final_test_iq_read=False,
        warning='Keep simultaneous indoor/outdoor pairs together. One file is not 180 independent sessions. '
        'Review exact views before labels; inventory does not authorize training.')
    write_new_json(output,report)
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();r=inventory(a.root,a.output)
    print(json.dumps(dict(captures=len(r['records']),publisher_sha256_verified=True,training_eligible=False)))


if __name__=='__main__':main()
