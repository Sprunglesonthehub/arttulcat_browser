# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.


import os
import re

import attr
import yaml
from mozilla_version.gecko import FirefoxVersion

from ..cli import BaseTryParser
from ..push import push_to_try, vcs

TARGET_TASKS = {
    "staging": "staging_release_builds",
    "release-sim": "release_simulation",
}


def read_file(path):
    with open(path) as fh:
        return fh.read()


class ReleaseParser(BaseTryParser):
    name = "release"
    arguments = [
        [
            ["-v", "--version"],
            {
                "metavar": "STR",
                "required": True,
                "action": "store",
                "type": FirefoxVersion.parse,
                "help": "The version number to use for the staging release.",
            },
        ],
        [
            ["--migration"],
            {
                "metavar": "STR",
                "action": "append",
                "dest": "migrations",
                "choices": [
                    "main-to-beta",
                    "beta-to-release",
                    "early-to-late-beta",
                    "release-to-esr",
                ],
                "help": "Migration to run for the release (can be specified multiple times).",
            },
        ],
        [
            ["--no-limit-locales"],
            {
                "action": "store_false",
                "dest": "limit_locales",
                "help": "Don't build a limited number of locales in the staging release.",
            },
        ],
        [
            ["--tasks"],
            {
                "choices": TARGET_TASKS.keys(),
                "default": "staging",
                "help": "Which tasks to run on-push.",
            },
        ],
    ]
    common_groups = ["push"]
    task_configs = ["disable-pgo", "worker-overrides", "existing-tasks"]

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.set_defaults(migrations=[])


def run(
    version,
    migrations,
    limit_locales,
    tasks,
    try_config_params=None,
    stage_changes=False,
    dry_run=False,
    message="{msg}",
    closed_tree=False,
    push_to_vcs=False,
):
    app_version = attr.evolve(version, beta_number=None, is_esr=False)

    files_to_change = {
        "browser/config/version.txt": f"{app_version}\n",
        "browser/config/version_display.txt": f"{version}\n",
        "config/milestone.txt": f"{app_version}\n",
        "mobile/android/version.txt": f"{version}\n",
    }
    with open("browser/config/version.txt") as f:
        current_version = FirefoxVersion.parse(f.read())
    format_options = {
        "current_major_version": current_version.major_number,
        "next_major_version": version.major_number,
        "current_weave_version": current_version.major_number + 2,
        "next_weave_version": version.major_number + 2,
    }

    if "beta-to-release" in migrations and "early-to-late-beta" not in migrations:
        migrations.append("early-to-late-beta")

    release_type = version.version_type.name.lower()
    if release_type not in ("beta", "release", "esr"):
        raise Exception(
            f"Can't do staging release for version: {version} type: {version.version_type}"
        )
    elif release_type == "esr":
        release_type += str(version.major_number)
    task_config = {"version": 2, "parameters": try_config_params or {}}
    task_config["parameters"].update(
        {
            "target_tasks_method": TARGET_TASKS[tasks],
            "optimize_target_tasks": True,
            "release_type": release_type,
        }
    )

    with open(
        os.path.join(vcs.path, "taskcluster/kinds/merge-automation/kind.yml")
    ) as f:
        migration_configs = yaml.safe_load(f)
    for migration in migrations:
        # pull out the behaviour-specific configuration
        actions = migration_configs["tasks"]["merge-automation"]["worker"]["actions"]
        behaviour_config = actions["by-behavior"][migration][0]
        # there is always one action in the behaviour config
        migration_config = list(behaviour_config.values())[0]
        for path, from_, to in migration_config["replacements"]:
            if path in files_to_change:
                contents = files_to_change[path]
            else:
                contents = read_file(path)
            from_ = from_.format(**format_options)
            to = to.format(**format_options)
            files_to_change[path] = contents.replace(from_, to)

        for path, from_, to in migration_config.get("regex-replacements", []):
            if path in files_to_change:
                contents = files_to_change[path]
            else:
                contents = read_file(path)
            from_regex = from_.format(**format_options)
            to = to.format(**format_options)
            files_to_change[path] = re.sub(from_regex, to, contents)

    if limit_locales:
        files_to_change["browser/locales/l10n-changesets.json"] = read_file(
            os.path.join(vcs.path, "browser/locales/l10n-onchange-changesets.json")
        )
        files_to_change["browser/locales/shipped-locales"] = "en-US\n" + read_file(
            os.path.join(vcs.path, "browser/locales/onchange-locales")
        )

    msg = f"staging release: {version}"
    return push_to_try(
        "release",
        message.format(msg=msg),
        stage_changes=stage_changes,
        dry_run=dry_run,
        closed_tree=closed_tree,
        try_task_config=task_config,
        files_to_change=files_to_change,
        push_to_vcs=push_to_vcs,
    )
