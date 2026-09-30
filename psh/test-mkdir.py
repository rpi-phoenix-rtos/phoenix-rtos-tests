import psh.tools.psh as psh
from psh.tools.randwrapper import TestRandom
from psh.tools.common import CHARS, assert_dir_created, assert_random_dirs, assert_deleted_rec, assert_present

ROOT_TEST_DIR = 'test_mkdir_dir'


def assert_visible(p, fname):
    files = psh.ls(p)
    msg = f"{fname} isn't visible in root directory"
    assert files == psh.ls(p, f'{fname}/..'), msg


def assert_mkdir_parents(p):
    """mkdir -p: create missing parents; an existing directory is not an error (POSIX)"""
    base = f'{ROOT_TEST_DIR}/parents'

    psh.assert_cmd(p, f'mkdir -p {base}/a/b/c', result='success', msg='mkdir -p did not create the missing parents')
    assert_present('c', psh.ls(p, f'{base}/a/b'), dir=True)

    psh.assert_cmd(p, f'mkdir -p {base}/a/b/c', result='success', msg='mkdir -p failed on an existing path')
    psh.assert_cmd(p, f'mkdir -p {ROOT_TEST_DIR}', result='success', msg='mkdir -p failed on an existing directory')
    psh.assert_cmd(p, f'mkdir -p {base}/x {base}/y/z', result='success', msg='mkdir -p failed with two operands')
    assert_present('z', psh.ls(p, f'{base}/y'), dir=True)

    # A file in the way is still an error, with or without -p
    psh.assert_cmd(p, f'touch {base}/file', result='success', msg='touch failed')
    psh.assert_prompt_after_cmd(p, f'mkdir -p {base}/file', result='fail')
    psh.assert_prompt_after_cmd(p, f'mkdir -p {base}/file/sub', result='fail')

    # Without -p the parent must exist
    psh.assert_prompt_after_cmd(p, f'mkdir {base}/missing/sub', result='fail')


@psh.run
def harness(p):
    random_wrapper = TestRandom(seed=1)

    assert_dir_created(p, ROOT_TEST_DIR)
    assert_visible(p, ROOT_TEST_DIR)

    assert_dir_created(p, f'{ROOT_TEST_DIR}/another_dir')
    # there are some targets, where max fname length equals 64
    assert_dir_created(p, f'{ROOT_TEST_DIR}/' + ''.join(CHARS[:50]))
    assert_dir_created(p, f'{ROOT_TEST_DIR}/' + ''.join(CHARS[50:]))

    assert_random_dirs(p, CHARS, f'{ROOT_TEST_DIR}/random/', random_wrapper, count=20)

    psh.assert_prompt_after_cmd(p, 'mkdir /', result='fail')
    psh.assert_prompt_after_cmd(p, f'mkdir {ROOT_TEST_DIR}', result='fail')

    assert_mkdir_parents(p)

    assert_deleted_rec(p, ROOT_TEST_DIR)
