#!/usr/bin/env python3
"""
esp_bytebeat_backup.py - create project backup zip with verification
Usage: python esp_bytebeat_backup.py [--project-root PATH] [--exclude PATH ...] [--dry-run]

Features:
- Intelligent backup with automatic verification
- Prevents empty backups (0 MB / 0 files)
- Verifies critical files are included
- Shows detailed statistics

Excludes:
- Folders starting with . (.vscode, .documents, .git, etc.)
- Build artifacts (build, install)
- Log and temporary files
"""
from __future__ import annotations
import argparse
import os
import sys
import zipfile
import datetime
import re
from typing import List, Tuple, Set


def find_project_root(script_path: str) -> str:
    """Find project root by going up from tools/backup/python-scripts"""
    return os.path.abspath(os.path.join(os.path.dirname(script_path), '..', '..', '..'))


def normalize_excludes(excludes: List[str]) -> List[str]:
    """Normalize path separators in exclude patterns"""
    out = []
    for ex in excludes:
        ex = ex.replace('/', os.sep).replace('\\', os.sep).strip(os.sep)
        out.append(ex)
    return out


def is_excluded(abs_path: str, project_root: str, excludes: List[str]) -> bool:
    """Check if path should be excluded from backup"""
    rel = os.path.relpath(abs_path, project_root)
    rel = rel.replace('\\', '/')
    
    # Don't exclude the root directory itself
    if rel == '.':
        return False
    
    # Check if any folder in the path starts with . (like .vscode, .documents, .git)
    # But only check directories, not files like .clangd
    if os.path.isdir(abs_path):
        path_parts = rel.split('/')
        for part in path_parts:
            if part != '.' and part.startswith('.'):
                return True
    
    # Check against explicit excludes
    for ex in excludes:
        ex_norm = ex.replace('\\', '/').rstrip('/')
        if rel == ex_norm or rel.startswith(ex_norm + '/'):
            return True
    return False


def is_file_excluded(filename: str, file_excludes: List[str]) -> bool:
    """Check if file should be excluded based on extension"""
    lower_name = filename.lower()
    for pattern in file_excludes:
        if lower_name.endswith(pattern.lower()):
            return True
    return False


def make_zip(project_root: str, dest_zip: str, excludes: List[str], file_excludes: List[str], verbose: bool = False) -> tuple:
    """Create zip archive of project with exclusions"""
    file_count = 0
    skipped_dirs = []
    skipped_files = []
    
    with zipfile.ZipFile(dest_zip, 'w', compression=zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(project_root):
            # Skip excluded directories
            if is_excluded(root, project_root, excludes):
                if verbose:
                    skipped_dirs.append(os.path.relpath(root, project_root))
                dirs[:] = []
                continue
            
            # Filter out directories that start with .
            original_dirs = dirs[:]
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            if verbose:
                for d in original_dirs:
                    if d.startswith('.'):
                        skipped_dirs.append(os.path.join(os.path.relpath(root, project_root), d))
            
            rel_root = os.path.relpath(root, project_root)
            if rel_root == '.':
                rel_root = ''
            
            for f in files:
                # Skip excluded file types
                if is_file_excluded(f, file_excludes):
                    if verbose:
                        skipped_files.append(os.path.join(rel_root, f))
                    continue
                
                absf = os.path.join(root, f)
                
                # Don't include the backup zip itself
                if os.path.abspath(absf) == os.path.abspath(dest_zip):
                    continue
                
                arcname = os.path.normpath(os.path.join(rel_root, f))
                zf.write(absf, arcname)
                file_count += 1
                if verbose and file_count <= 10:
                    print(f'  Added: {arcname}')
    
    if verbose:
        print(f'\nSkipped {len(skipped_dirs)} directories')
        if skipped_dirs[:5]:
            print('  First 5:', ', '.join(skipped_dirs[:5]))
        print(f'Skipped {len(skipped_files)} files')
        if skipped_files[:5]:
            print('  First 5:', ', '.join(skipped_files[:5]))
    
    return dest_zip, file_count


def get_zip_size_mb(zip_path: str) -> float:
    """Get zip file size in MB"""
    return os.path.getsize(zip_path) / (1024 * 1024)


def get_critical_files() -> List[str]:
    """Return list of critical files that must be in backup"""
    return [
        'CMakeLists.txt',
        'test_apps/console/CMakeLists.txt',
        'test_apps/console/main/main.cpp',
        'test_apps/console/OmniBox_DAW.html',
        'tools/backup/esp_bytebeat_backup.bat',
        'tools/backup/python-scripts/esp_bytebeat_backup.py'
    ]


def verify_backup(zip_path: str, project_root: str) -> Tuple[bool, List[str], dict]:
    """Verify backup integrity and critical files"""
    issues = []
    stats = {
        'total_files': 0,
        'total_size_mb': 0.0,
        'has_main': False,
        'has_test_apps': False,
        'has_tools': False,
        'critical_missing': []
    }
    
    # Check if file exists
    if not os.path.exists(zip_path):
        issues.append('Backup file does not exist')
        return False, issues, stats
    
    # Check file size
    stats['total_size_mb'] = get_zip_size_mb(zip_path)
    if stats['total_size_mb'] < 0.01:
        issues.append(f'Backup is empty or too small ({stats["total_size_mb"]:.2f} MB)')
        return False, issues, stats
    
    # Read ZIP contents
    try:
        with zipfile.ZipFile(zip_path, 'r') as zf:
            files = zf.namelist()
            stats['total_files'] = len(files)
            
            # Check if backup is empty
            if stats['total_files'] == 0:
                issues.append('Backup contains 0 files')
                return False, issues, stats
            
            # Check for main directories
            stats['has_main'] = any(f.startswith('test_apps/console/main/') for f in files)
            stats['has_test_apps'] = any(f.startswith('test_apps/') for f in files)
            stats['has_tools'] = any(f.startswith('tools/') for f in files)
            
            # Check critical files
            critical_files = get_critical_files()
            for critical in critical_files:
                # Normalize path separators for cross-platform
                critical_normalized = critical.replace('\\', '/')
                if critical_normalized not in files:
                    stats['critical_missing'].append(critical)
            
            # Add issues for missing critical components
            if not stats['has_main']:
                issues.append('Missing test_apps/console/main/ directory')
            if not stats['has_test_apps']:
                issues.append('Missing test_apps/ directory')
            if stats['critical_missing']:
                issues.append(f'Missing {len(stats["critical_missing"])} critical files')
    
    except zipfile.BadZipFile:
        issues.append('Backup file is corrupted (not a valid ZIP)')
        return False, issues, stats
    except Exception as e:
        issues.append(f'Error reading backup: {str(e)}')
        return False, issues, stats
    
    # Determine if backup is valid
    is_valid = len(issues) == 0 and stats['total_files'] > 0 and stats['total_size_mb'] > 0.01
    
    return is_valid, issues, stats


def print_verification_report(is_valid: bool, issues: List[str], stats: dict) -> None:
    """Print detailed verification report"""
    print()
    print('=' * 70)
    print('BACKUP VERIFICATION')
    print('=' * 70)
    
    if is_valid:
        print('[OK] Backup is VALID')
    else:
        print('[FAIL] Backup FAILED verification')
    
    print()
    print(f'Files archived: {stats["total_files"]}')
    print(f'Archive size: {stats["total_size_mb"]:.2f} MB')
    print()
    print('Directory structure:')
    print(f'  {"[OK]" if stats["has_main"] else "[FAIL]"} test_apps/console/main/')
    print(f'  {"[OK]" if stats["has_test_apps"] else "[FAIL]"} test_apps/')
    print(f'  {"[OK]" if stats["has_tools"] else "[FAIL]"} tools/')
    
    if stats['critical_missing']:
        print()
        print(f'[FAIL] Missing {len(stats["critical_missing"])} critical files:')
        for missing in stats['critical_missing'][:5]:
            print(f'  - {missing}')
        if len(stats['critical_missing']) > 5:
            print(f'  ... and {len(stats["critical_missing"]) - 5} more')
    
    if issues:
        print()
        print('Issues found:')
        for issue in issues:
            print(f'  [FAIL] {issue}')
    
    print('=' * 70)


def main() -> int:
    parser = argparse.ArgumentParser(
        description='Create a project ZIP backup excluding dot-folders and build artifacts.'
    )
    parser.add_argument(
        '--project-root',
        help='Project root path (defaults to three levels up from this script).',
        default=None
    )
    parser.add_argument(
        '--exclude',
        action='append',
        help='Additional relative paths to exclude (can be provided multiple times).',
        default=[]
    )
    parser.add_argument(
        '--dry-run',
        action='store_true',
        help='Show what would be done without creating files.'
    )
    parser.add_argument(
        '--verbose',
        action='store_true',
        help='Show detailed information about what is being backed up.'
    )
    args = parser.parse_args()

    script_path = __file__
    project_root = args.project_root or find_project_root(script_path)
    project_root = os.path.abspath(project_root)

    if not os.path.isdir(project_root):
        print(f'ERROR: Project root not found: {project_root}', file=sys.stderr)
        return 2

    project_name = os.path.basename(project_root)
    # Remove version number from project name if present
    project_name_no_version = re.sub(r'-\d+\.\d+\.\d+$', '', project_name)
    safe_project = project_name_no_version.replace(' ', '_')

    # Default exclusions (dot-folders are handled separately in is_excluded)
    default_excludes = [
        'build',
        'install',
        'test_apps/console/build',
        'test_apps/console/managed_components',
        'tools/backup/backups',
        '__pycache__',
        'logs',
        'dist',
        'node_modules'
    ]
    
    # File types to exclude (only temp/log files, keep .md and code files)
    default_file_excludes = ['.log', '.tmp', '.bak']
    
    excludes = default_excludes + (args.exclude or [])
    excludes = normalize_excludes(excludes)

    # Create timestamp: YYYY-MM-DD_HH-MM-SS
    ts = datetime.datetime.now().strftime('%Y-%m-%d_%H-%M-%S')
    zip_filename = f'{ts}_{safe_project}.zip'

    project_backup_dir = os.path.join(project_root, 'tools', 'backup', 'backups')
    os.makedirs(project_backup_dir, exist_ok=True)
    project_zip_path = os.path.join(project_backup_dir, zip_filename)

    print('=' * 70)
    print('ESP32 Bytebeat Project Backup')
    print('=' * 70)
    print(f'Project: {project_name}')
    print(f'Project root: {project_root}')
    print(f'Archive name: {zip_filename}')
    print(f'Backup dir: {project_backup_dir}')
    print()
    print('Exclusions:')
    print('  - Dot-folders: .vscode, .documents, .git (but keeps .clangd files)')
    for ex in default_excludes[:5]:
        print(f'  - {ex}')
    if len(default_file_excludes) > 0:
        print('  - File types: ' + ', '.join(default_file_excludes))
    print('=' * 70)

    if args.dry_run:
        print('\nDRY RUN: No files will be created.')
        return 0

    print('\nCreating backup...')
    if args.verbose:
        print('First 10 files added:')
    
    try:
        project_zip_path, file_count = make_zip(project_root, project_zip_path, excludes, default_file_excludes, args.verbose)
        zip_size = get_zip_size_mb(project_zip_path)
    except Exception as e:
        print()
        print('=' * 70)
        print('ERROR: Backup creation failed!')
        print('=' * 70)
        print(f'Error: {str(e)}')
        print('=' * 70)
        return 3

    # Immediate sanity check
    if file_count == 0 or zip_size < 0.01:
        print()
        print('=' * 70)
        print('WARNING: Backup appears to be empty!')
        print('=' * 70)
        print(f'Files archived: {file_count}')
        print(f'Archive size: {zip_size:.2f} MB')
        print()
        print('This indicates a problem with the backup configuration.')
        print('The backup file will be kept for debugging.')
        print('=' * 70)
        return 4

    print()
    print('=' * 70)
    print('Backup created successfully!')
    print('=' * 70)
    print(f'Files archived: {file_count}')
    print(f'Archive size: {zip_size:.2f} MB')
    print(f'Location: {project_zip_path}')
    
    # Verify backup integrity
    print()
    print('Verifying backup integrity...')
    is_valid, issues, stats = verify_backup(project_zip_path, project_root)
    
    print_verification_report(is_valid, issues, stats)
    
    if not is_valid:
        print()
        print('[FAIL] WARNING: Backup verification failed!')
        print('The backup file has been created but may be incomplete.')
        print('Please review the issues above.')
        return 5
    
    print()
    print('[OK] Backup completed and verified successfully!')
    print('=' * 70)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
