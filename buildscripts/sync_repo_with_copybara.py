"""Module for syncing a repo with Copybara and setting up configurations."""
import argparse
import subprocess
import os
import sys

from typing import Optional
from github import GithubIntegration

from buildscripts.util.read_config import read_config_file
from evergreen.api import RetryingEvergreenApi


def run_command(command):  # noqa: D406,D407
    """
    Execute a shell command and return its standard output (`stdout`).

    Args:
        command (str): The shell command to be executed.

    Returns:
        str: The standard output of the executed command.

    Raises:
        subprocess.CalledProcessError: If the command execution fails.

    """
    try:
        return subprocess.run(command, shell=True, check=True, text=True,
                              capture_output=True).stdout
    except subprocess.CalledProcessError as err:
        print(f"Error while executing: '{command}'.\n{err}\nStandard Error: {err.stderr}")
        raise


def create_mongodb_bot_gitconfig():
    """Create the mongodb-bot.gitconfig file with the desired content."""

    content = """
    [user]
        name = MongoDB Bot
        email = mongo-bot@mongodb.com
    """

    gitconfig_path = os.path.expanduser("~/mongodb-bot.gitconfig")

    with open(gitconfig_path, 'w') as file:
        file.write(content)

    print("mongodb-bot.gitconfig file created.")


def get_installation_access_token(app_id: int, private_key: str,
                                  installation_id: int) -> Optional[str]:  # noqa: D406,D407,D413
    """
    Obtain an installation access token using JWT.

    Args:
    - app_id (int): The application ID for GitHub App.
    - private_key (str): The private key associated with the GitHub App.
    - installation_id (int): The installation ID of the GitHub App for a particular account.

    Returns:
    - Optional[str]: The installation access token. Returns `None` if there's an error obtaining the token.

    """
    integration = GithubIntegration(app_id, private_key)
    auth = integration.get_access_token(installation_id)

    if auth:
        return auth.token
    else:
        print("Error obtaining installation token")
        return None


def send_failure_message_to_slack(expansions):
    """
    Send a failure message to a specific Slack channel when the Copybara task fails.

    :param expansions: Dictionary containing various expansion data.
    """
    current_version_id = expansions.get("version_id", None)
    error_msg = (
        "Evergreen task '* Copybara Sync Between Repos' failed\n"
        f"For more details: <https://spruce.mongodb.com/version/{current_version_id}|here>.")

    evg_api = RetryingEvergreenApi.get_api(config_file=".evergreen.yml")
    evg_api.send_slack_message(
        target="#sdp-triager",
        msg=error_msg,
    )


def main():
    """Clone the Copybara repo, build its Docker image, and set up and run migrations."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--expansion-file", dest="expansion_file", type=str,
                        help="Location of expansions file generated by evergreen.")

    args = parser.parse_args()

    # Check if the copybara directory already exists
    if os.path.exists('copybara'):
        print("Copybara directory already exists.")
    else:
        run_command("git clone https://github.com/10gen/copybara.git")

    # Navigate to the Copybara directory and build the Copybara Docker image
    run_command("cd copybara && docker build --rm -t copybara .")

    # Read configurations
    expansions = read_config_file(args.expansion_file)

    access_token_copybara_syncer = get_installation_access_token(
        expansions["app_id_copybara_syncer"], expansions["private_key_copybara_syncer"],
        expansions["installation_id_copybara_syncer"])

    # Create the mongodb-bot.gitconfig file as necessary.
    create_mongodb_bot_gitconfig()

    current_dir = os.getcwd()
    git_destination_url_with_token = f"https://x-access-token:{access_token_copybara_syncer}@github.com/mongodb/mongo.git"

    

    # Set up the Docker command and execute it
    # --last-rev Defines the last revision that was migrated to the destination during the initial synchronization between repositories using Copybara
    docker_cmd = [
        "docker run",
        "-v ~/.ssh:/root/.ssh",
        "-v ~/mongodb-bot.gitconfig:/root/.gitconfig",
        f'-v "{current_dir}/copybara.sky":/usr/src/app/copy.bara.sky',
        "-e COPYBARA_CONFIG='copy.bara.sky'",
        "-e COPYBARA_SUBCOMMAND='migrate'",
        f"-e COPYBARA_OPTIONS='-v --last-rev=0fd0cc3 --git-destination-url={git_destination_url_with_token}'",
        "copybara copybara",
    ]

    try:
        run_command(" ".join(docker_cmd))
    except subprocess.CalledProcessError as err:
        error_message = str(err.stderr)
        acceptable_error_messages = [
            # Indicates the two repositories are identical
            "No new changes to import for resolved ref",
            # Indicates differences exist but no changes affect the destination, for example: exclusion rules
            "Iterative workflow produced no changes in the destination for resolved ref",
        ]

        if any(acceptable_message in error_message
               for acceptable_message in acceptable_error_messages):
            return

        # Send a failure message to #sdp-triager if the Copybara sync task fails.
        send_failure_message_to_slack(expansions)


if __name__ == "__main__":
    main()
