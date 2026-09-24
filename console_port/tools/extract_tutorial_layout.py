#!/usr/bin/env python3
"""Extract original tutorial MapOptions and ApplySchematic placements unchanged."""
from pathlib import Path
import sys,xml.etree.ElementTree as ET
r=Path(__file__).resolve().parents[1]
root=ET.parse(r/'assets/tutorial/GameRules.xml').getroot();ns={'g':'GameRulesDefinition.xsd'};opts=root.find('g:MapOptions',ns)
a=opts.attrib
out='''// Generated from original tutorial GameRules.xml; no substitute structures.
#include "TutorialSchematics.h"
namespace console {
const std::int64_t tutorialSeed='''+a['seed']+'''ll;
const std::array<int,3> tutorialSpawn={'''+','.join(a['spawn'+v] for v in 'XYZ')+'''};
const std::vector<TutorialPlacement> tutorialPlacements={
'''
for rule in opts.findall('g:ApplySchematic',ns):
 d=rule.attrib
 if d.get('rot','0')!='0':raise ValueError('Rotated tutorial placement needs the original rotation adapter')
 # ApplySchematicRuleDefinition rounds odd source coordinates downward to even.
 xyz=[int(d[k])//2*2 for k in ('x','y','z')];xyz[1]=max(0,xyz[1])
 out+=' {"'+d['filename']+'",'+','.join(str(v) for v in xyz)+'},\n'
out+='};\n}\n';Path(sys.argv[1]).write_text(out)
