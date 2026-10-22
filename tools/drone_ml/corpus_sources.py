"""Read-only source review for expanded, weakly labelled drone RF training."""
import hashlib
import io
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import time
import xml.etree.ElementTree as ET
import zipfile
import h5py
import numpy as np
from .common import identity, read_source, sha256, stamp, write_json
from .drff import vector, scalar, text

DRFF_LABELS = dict(mavicAir2='DJI_Air_2', mavicAir2s='DJI_Air_2S', mini4PRO='DJI_Mini_4_Pro',
                   mini5PRO='DJI_Mini_5_Pro', mavic3='DJI_Mavic_3', mini3pro='DJI_Mini_3_Pro',
                   mavic3C='DRFF_mavic3C_context', mavic3S='DRFF_mavic3S_context')
TAMPERE = dict(DJI_inspire_2='DJI_Inspire_2', DJI_matrice_100='DJI_Matrice_100',
    DJI_matrice_210='DJI_Matrice_210', DJI_mavic_mini='DJI_Mavic_Mini', DJI_mavic_pro='DJI_Mavic_Pro',
    DJI_phantom_4='DJI_Phantom_4', DJI_phantom_4_pro_plus='DJI_Phantom_4_Pro_Plus',
    Parrot_disco='Parrot_Disco', Parrot_mambo_control='Parrot_Mambo', Parrot_mambo_video='Parrot_Mambo',
    Yuneec_typhoon_h='Yuneec_Typhoon_H')
LEUVEN = {'mini2vid.zip':'DJI_Mini_2', 'mini2RC.zip':'DJI_Mini_2',
          'inspire2Vid.zip':'DJI_Inspire_2', 'inspire2RC.zip':'DJI_Inspire_2',
          'matricevid.zip':'DJI_Matrice_300', 'matriceRC.zip':'DJI_Matrice_300', 'SJRC_pro.zip':'SJRC_F11_Pro'}
RFUAV = {'DJI AVATA2':'DJI_Avata_2', 'DJI FPV COMBO':'DJI_FPV', 'DJI MAVIC3 PRO':'DJI_Mavic_3_Pro',
         'DJI MINI3':'DJI_Mini_3'}
# DAUTEL token is deliberately not silently corrected to AUTEL; reviewed XML decides exclusion.


def rar_bytes(path, member=None, limit=2_000_000, prefix=False):
    if not 0 < limit <= 128_000_000:
        raise ValueError('archive read budget')
    command = ['unrar', 'lb', str(path)] if member is None else ['unrar', 'p', '-inul', str(path), member]
    p = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    output, deadline = bytearray(), time.monotonic() + 60
    try:
        with selectors.DefaultSelector() as sel:
            sel.register(p.stdout, selectors.EVENT_READ)
            while len(output) < limit:
                if time.monotonic() > deadline:
                    raise TimeoutError('archive read exceeded 60 seconds')
                if not sel.select(.5):
                    continue
                chunk = os.read(p.stdout.fileno(), min(65536, limit-len(output)))
                if not chunk:
                    break
                output.extend(chunk)
        if prefix:
            if len(output) != limit:
                raise ValueError('short archive prefix')
        elif len(output) == limit or p.wait(timeout=5) != 0:
            raise ValueError('archive output invalid or over budget')
        return bytes(output)
    finally:
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=2)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        p.stdout.close()


def xml_metadata(raw):
    if b'<!DOCTYPE' in raw.upper() or b'<!ENTITY' in raw.upper():
        raise ValueError('XML entities forbidden')
    root = ET.fromstring(raw)
    if root.tag != 'SignalHoundIQFile' or root.findtext('DataType') != 'Complex Float':
        raise ValueError('unsupported RFUAV format')
    m = {k: root.findtext(k) for k in ('Drone','DeviceType','SerialNumber','DataType','IQFileName')}
    for k in ('SampleRate','CenterFrequency','SampleCount'):
        m[k] = float(root.findtext(k))
        if not np.isfinite(m[k]) or m[k] <= 0:
            raise ValueError('invalid XML numeric value')
    return m


def drff_metadata(path):
    with read_source(path) as f, h5py.File(f, 'r') as h:
        centers = [scalar(h,k) for k in ('CenterFreq','CenterFrequence') if k in h]
        if not centers or len(set(centers)) != 1:
            raise ValueError('conflicting frequency fields')
        i,q = vector(h,'RF0_I'),vector(h,'RF0_Q')
        if i.shape != q.shape or len(i.shape)!=2 or min(i.shape)!=1 or i.dtype.kind!='f' or q.dtype.kind!='f':
            raise ValueError('unsupported DRFF IQ layout')
        d = vector(h,'C')
        condition = int(text(h,'C').removeprefix('c')) if d.attrs.get('MATLAB_class') == b'char' else scalar(h,'C')
        if condition != int(condition) or not 1 <= condition <= 26:
            raise ValueError('noninteger or unsupported condition')
        condition = int(condition)
        if not 0 < centers[0] <= 10_000_000_000:
            raise ValueError('invalid centre frequency')
        return dict(target=text(h,'TD'),state=text(h,'State'),receiver=text(h,'U'),day=text(h,'D'),
                    condition=condition,rate_hz=scalar(h,'Fs'),center_hz=centers[0],total_samples=i.size)


def unit_partition(model, unit):
    if model == 'mini3pro': return 'unknown_calibration'
    if model in ('mavic3C','mavic3S'): return 'unknown_test_locked'
    if model in ('mavic3','mini5PRO'): return 'train'
    if unit <= 2: return 'train'
    if unit <= 4: return 'calibration'
    return 'test_locked'


def inspect(root, inventory_path, output, leuven_members=4, rfuav_packs=3):
    if not 1 <= leuven_members <= 32 or not 1 <= rfuav_packs <= 16:
        raise ValueError('invalid review sampling budget')
    root,output = Path(root).resolve(),Path(output).resolve()
    if root == output or root in output.parents or not os.statvfs(root).f_flag & os.ST_RDONLY:
        raise ValueError('requires read-only sources and separate output')
    inventory=json.loads(Path(inventory_path).read_text())
    rows, decisions = [], []
    def add(base, **fields):
        row=dict(source=base['source'], source_stat=stamp(base['source']), dataset=base['dataset'],
                 negative_eligible=False, label_scope='publisher_recording_context_not_verified_window_or_emitter',
                 continuity_verified=False, independent_deployment_verified=False, **fields)
        row['recording_id']=identity(row['source'],row.get('member'))
        rows.append(row)
    for base in inventory['records']:
        path=Path(base['source'])
        if path.is_symlink() or root not in path.resolve().parents:
            raise ValueError('source outside read-only root')
        before=stamp(path)
        # Compare stable original statistics to the earlier inventory; mount inode/device may change.
        if before[2:] != base['source_stat'][2:]:
            raise ValueError('source changed since inventory: '+str(path))
        decision=dict(source=str(path),dataset=base['dataset'],use='excluded',reason='unsupported_or_unverified_aircraft_identity')
        ds=base['dataset']
        try:
            if ds=='DRFF-R2':
                folder=path.parent.name
                if folder not in ('dataset1-single_drone_states','dataset3-single_drone_hover','dataset4-single_drone_dual_frequency'):
                    decision['reason']='mixture_or_environment_not_clean_model_truth'
                else:
                    match=re.fullmatch(r'(.+)_(\d+)_([^_]+)_c(\d+)_(u\d+)_(d\d+)\.mat',path.name)
                    if not match or match[1] not in DRFF_LABELS:
                        raise ValueError('unsupported filename')
                    model,unit=match[1],int(match[2]); m=drff_metadata(path)
                    if (m['target']!=f'{model}_{unit}' or m['state']!=match[3] or m['condition']!=int(match[4])
                        or m['receiver']!=match[5] or m['day']!=match[6] or m['rate_hz']!=100_000_000):
                        raise ValueError('filename and embedded metadata disagree')
                    part=unit_partition(model,unit)
                    add(base,adapter='drff',metadata=m,label=DRFF_LABELS[model],partition=part,
                        split_group=f'DRFF-R2:{model}:aircraft_unit_{unit}',
                        validation_support='publisher_cross_unit' if model in ('mavicAir2','mavicAir2s','mini4PRO') else 'none',
                        physical_unit=f'{model}_{unit}', receiver=m['receiver'],day=m['day'],role='drone_controller_context',
                        rate_hz=int(m['rate_hz']),center_hz=m['center_hz'],total_samples=m['total_samples'],
                        label_evidence='filename_TD_State_C_U_D_agree',prior_exposure='earlier_benchmark_test' if unit==5 else 'development_or_unassessed')
                    decision.update(use=part,reason='publisher_metadata_checked')
            elif ds=='Tampere_DroneRF':
                match=re.fullmatch(r'(.+)_(2G|5G)(?:_\dof\d)?\.bin',path.name)
                if not match or match[1] not in TAMPERE: raise ValueError('unsupported filename')
                if match[1]=='Parrot_mambo_control': raise ValueError('controller_only_not_aircraft_model_training')
                rate=120_000_000 if match[2]=='2G' else 200_000_000
                if before[2] != (480_000_000 if rate==120_000_000 else 400_000_000):
                    raise ValueError('file size differs from publisher')
                label=TAMPERE[match[1]]
                add(base,adapter='ci16',label=label,partition='train',split_group='unresolved_physical_unit:'+label,
                    validation_support='none',physical_unit=None,receiver='publisher_Tampere',day=None,
                    role='controller_only' if 'mambo_control' in path.name else 'video_only' if 'mambo_video' in path.name else 'drone_controller_context',
                    rate_hz=rate,center_hz=2440000000 if rate==120000000 else 5700000000 if 'Yuneec' in path.name else 5800000000,
                    total_samples=before[2]//4,label_evidence='publisher_Zenodo_4264467_filename_format')
                decision.update(use='train',reason='single_unresolved_unit_all_parts_bands_grouped_no_independent_test')
            elif ds=='KULeuven_DroneRF' and path.name in LEUVEN:
                label=LEUVEN[path.name]
                if 'RC' in path.name and path.name!='SJRC_pro.zip': raise ValueError('controller_only_not_aircraft_model_training')
                # Bounded representative membership fixed before signal inspection. All members stay one unit group.
                members=[x for x in base['members'] if x['name'].endswith('.mat')]
                chosen=[members[i] for i in np.unique(np.linspace(0,len(members)-1,min(leuven_members,len(members)),dtype=int))]
                for member in chosen:
                    if member['bytes']>512*1024*1024: raise ValueError('oversized MAT member')
                    add(base,adapter='leuven',member=member['name'],member_bytes=member['bytes'],member_crc32=member['crc32'],
                        label=label,partition='train',split_group='unresolved_physical_unit:'+label,validation_support='none',
                        physical_unit=None,receiver='publisher_Leuven_X310',day=None,role='controller_only' if 'RC' in path.name else 'video_or_combined_context',
                        rate_hz=100000000,center_hz=2440000000,total_samples=None,label_evidence='local_publisher_README_and_archive_name')
                decision.update(use='train',reason=f'up_to_{leuven_members}_members_all_sessions_conservatively_grouped')
            elif ds=='RFUAV':
                names=rar_bytes(path).decode('utf-8').splitlines()
                xmls=sorted(n for n in names if n.endswith('.xml'))
                inspected=[]
                # Inspect all available pack XML labels, never execute dataset scripts.
                for member in xmls:
                    raw=rar_bytes(path,member,65536); m=xml_metadata(raw)
                    inspected.append(dict(member=member,metadata=m,sha256=hashlib.sha256(raw).hexdigest()))
                decision['xml_review']=inspected
                norm=lambda s: re.sub('[^a-z0-9]','',s.lower())
                if any(norm(x['metadata']['Drone'])!=norm(path.stem) for x in inspected):
                    decision['reason']='filename_XML_target_conflict'
                elif path.stem=='DJI MINI4 PRO':
                    decision['reason']='cross_dataset_aircraft_identity_unresolved_keep_DRFF_holdouts_isolated'
                elif path.stem in RFUAV:
                    chosen=[inspected[i] for i in np.unique(np.linspace(0,len(inspected)-1,min(rfuav_packs,len(inspected)),dtype=int))] if inspected else []
                    for x in chosen:
                        member=x['member'][:-4]+'_0-1s.iq';m=x['metadata'];rate=int(m['SampleRate'])
                        if member not in names or rate!=m['SampleRate'] or not 20000000<=rate<=200000000:
                            raise ValueError('unsupported IQ member or sample rate')
                        add(base,adapter='rfuav',member=member,metadata=m,xml_sha256=x['sha256'],
                            label=RFUAV[path.stem],partition='train',split_group='unresolved_physical_unit:'+RFUAV[path.stem],
                            validation_support='none',physical_unit=None,receiver=m['DeviceType'],day=None,role='drone_controller_context',
                            rate_hz=rate,center_hz=m['CenterFrequency'],total_samples=min(rate,int(m['SampleCount'])),
                            label_evidence='archive_name_XML_Drone_agree_publisher_float32_reader')
                    decision.update(use='train',reason=f'up_to_{rfuav_packs}_packs_no_independent_units')
                else: decision['reason']='controller_equipment_or_unresolved_aircraft_alias'
            elif ds=='FPV_Analog_Video':
                decision['reason']='sweep_capture_equipment_labels_not_aircraft_models_continuity_format_need_review'
        except (ValueError,KeyError,OSError,ET.ParseError) as e:
            # Never keep partial rows for a source with a detected label/format conflict.
            rows[:]=[r for r in rows if r['source']!=str(path)]
            decision.update(use='excluded',reason=str(e))
        if stamp(path)!=before: raise ValueError('source changed during review')
        decisions.append(decision)
    groups={}
    for r in rows:
        g=groups.setdefault(r['split_group'],dict(partition=r['partition'],label=r['label']))
        if g != dict(partition=r['partition'],label=r['label']): raise ValueError('group split collision')
    output.mkdir(parents=True,exist_ok=False)
    write_json(output/'split_plan.json',dict(schema='rfmon.drone_ml.expanded_split.v1',groups=groups,
        final_partitions_locked=True,prior_test_exposure_disclosed=True))
    report=dict(schema='rfmon.drone_ml.expanded_review.v1',records=rows,source_decisions=decisions,
        source_root=str(root),source_inventory_sha256=sha256(inventory_path),
        split_sha256=sha256(output/'split_plan.json'),source_stats_preserved=True,
        release_validation_eligible=False,negative_truth_available=False)
    write_json(output/'review.json',report)
    return report
