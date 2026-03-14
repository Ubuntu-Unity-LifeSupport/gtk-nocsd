#!/usr/bin/env python3

# Copyright (C) 2020, 2023, UBports Foundation.
# Author: Ratchanan Srirattanamet
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

# This script automates upgrading upstream tarball when using Git snapshot
# for projects that uses GitHub, tailored to work in the way UBports CI
# works.

# Customize values for each package here. Although, if you modify version
# template, you might also need to take a look at the parsing regex below.

PACKAGE_NAME = 'gtk-nocsd'
GITHUB_UPSTREAM_PROJECT = 'MorsMortium/GTK-NoCSD'
GITHUB_UPSTREAM_BRANCH = 'main'
DCH_UPSTREAM_VERSION_TEMPLATE = '0~{gitdate}+{githash}'
DCH_DEBIAN_REVISION_TEMPLATE = '1'

import argparse
import os
import re
import shutil
import subprocess

import dateutil.parser
import requests

if not os.path.exists('./debian/changelog'):
    print('Please run this script outside debian/ directory.')
    quit(1)

arg_parser = argparse.ArgumentParser()
arg_parser.add_argument('-f', '--force', action='store_true',
                        help="Ignore failure to parse debian/changelog and always update it. "
                             "Won't be able to detect version collision.")
arg_parser.add_argument('--skip-download-and-extract', action='store_true',
                        help="If new version is found, skip download & extract of the new tarball.")

args = arg_parser.parse_args()

# Parse current version
dch_version = subprocess.run(
    ['dpkg-parsechangelog', '--show-field', 'Version'],
    capture_output=True, text=True, check=True).stdout
dch_version = dch_version.strip()

dch_match = re.match(
    # 0.1.0   ~ 20201020                                        + 8883e9a              -0ubports20.04.1
    r'[0-9.]+\~(?P<gitdate>[0-9]{8})(\.(?P<gitdaterev>[0-9]+))?\+(?P<githash>[0-9a-f]+)-.*',
    dch_version
)

if dch_match is None:
    if not args.force:
        print('Cannot parse current version from debian/changelog.')
        quit(2)

    dch_gitdate = ''
    dch_gitdaterev = 0
    dch_githash = ''
else:
    dch_gitdate = dch_match['gitdate']
    dch_githash = dch_match['githash']
    if dch_match['gitdaterev'] is None:
        dch_gitdaterev = 0
    else:
        dch_gitdaterev = int(dch_match['gitdaterev'])

# Retrieve current upstream version using GitHub's API.

github_branch_url = 'https://codeberg.org/api/v1/repos/{}/branches/{}' \
    .format(GITHUB_UPSTREAM_PROJECT, GITHUB_UPSTREAM_BRANCH)

github_branch_res = requests.get(github_branch_url)
github_branch = github_branch_res.json()

github_githashfull = github_branch['commit']['id']
github_githash = github_githashfull[0:7]
if github_githash == dch_githash:
    print("Already packaging current version, do nothing.")
    quit(0)

github_date = github_branch['commit']['timestamp']
github_gitdate = dateutil.parser.parse(github_date).strftime('%Y%m%d')

if github_gitdate == dch_gitdate:
    github_gitdatewithrev = '{}.{}'.format(github_gitdate, dch_gitdaterev + 1)
else:
    github_gitdatewithrev = github_gitdate

github_upstreamversion = DCH_UPSTREAM_VERSION_TEMPLATE.format(
    githash = github_githash,
    gitdate = github_gitdatewithrev
)
github_version = '{}-{}'.format(github_upstreamversion, DCH_DEBIAN_REVISION_TEMPLATE)

print('New version found, updating debian/changelog and ubports.source_location')

# Set the new version in debian/changelog
subprocess.run(
    ['dch', '--newversion', github_version, 'New upstream git snapshot'],
    check=True
)

# Also update ubports.source_location
github_codeload = 'https://codeberg.org/api/v1/repos/{}/archive/{}' \
    .format(GITHUB_UPSTREAM_PROJECT, github_githashfull + '.tar.gz')
github_origtarname = '{}_{}.orig.tar.gz' \
    .format(PACKAGE_NAME, github_upstreamversion)

with open('./debian/ubports.source_location', 'w') as f:
    f.write("{}\n{}\n".format(github_codeload, github_origtarname))

# Finally, for convenience, download the new tarball and extract it.
# https://stackoverflow.com/a/39217788
if not args.skip_download_and_extract:
    print('Downloading the new orig tarball and extract it')

    with requests.get(github_codeload, stream=True) as r:
        with open('../' + github_origtarname, 'wb') as f:
            shutil.copyfileobj(r.raw, f)

    subprocess.run(['origtargz', '--clean'], check=True)
    subprocess.run(['origtargz', '--unpack'], check=True)
