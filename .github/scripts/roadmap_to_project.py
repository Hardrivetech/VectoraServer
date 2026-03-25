import os
import sys
import yaml
import requests

GITHUB_API = "https://api.github.com"
REPO = os.environ.get("GITHUB_REPOSITORY")
TOKEN = os.environ.get("GH_TOKEN")
PROJECT_ID = os.environ.get("PROJECT_ID")
STATUS_FIELD_ID = os.environ.get("STATUS_FIELD_ID")
BACKLOG_OPTION_ID = os.environ.get("BACKLOG_OPTION_ID")
IN_PROGRESS_OPTION_ID = os.environ.get("IN_PROGRESS_OPTION_ID")
REVIEW_OPTION_ID = os.environ.get("REVIEW_OPTION_ID")
DONE_OPTION_ID = os.environ.get("DONE_OPTION_ID")
ROADMAP_FILE = os.environ.get("ROADMAP_FILE", ".github/example-roadmap.yml")

HEADERS = {
    "Authorization": f"Bearer {TOKEN}",
    "Accept": "application/vnd.github+json"
}

def create_issue(title, body, labels):
    url = f"{GITHUB_API}/repos/{REPO}/issues"
    data = {"title": title, "body": body, "labels": labels}
    resp = requests.post(url, json=data, headers=HEADERS)
    resp.raise_for_status()
    return resp.json()["number"], resp.json()["node_id"]

def add_to_project(issue_node_id):
    query = """
    mutation($project:ID!, $item:ID!) {
      addProjectV2ItemById(input: {projectId: $project, contentId: $item}) {
        item { id }
      }
    }
    """
    variables = {"project": PROJECT_ID, "item": issue_node_id}
    resp = requests.post(
        f"{GITHUB_API}/graphql",
        json={"query": query, "variables": variables},
        headers=HEADERS
    )
    resp.raise_for_status()
    return resp.json()["data"]["addProjectV2ItemById"]["item"]["id"]

def set_status(item_id, status):
    query = """
    mutation($project:ID!, $item:ID!, $field:ID!, $option:String!) {
      updateProjectV2ItemFieldValue(input: {
        projectId: $project,
        itemId: $item,
        fieldId: $field,
        value: { singleSelectOptionId: $option }
      }) {
        projectV2Item { id }
      }
    }
    """
    status_map = {
        "backlog": BACKLOG_OPTION_ID,
        "in progress": IN_PROGRESS_OPTION_ID,
        "review": REVIEW_OPTION_ID,
        "done": DONE_OPTION_ID
    }
    # Default to backlog if status is missing or unknown
    option_id = status_map.get(str(status).strip().lower(), BACKLOG_OPTION_ID)
    variables = {
        "project": PROJECT_ID,
        "item": item_id,
        "field": STATUS_FIELD_ID,
        "option": option_id
    }
    resp = requests.post(
        f"{GITHUB_API}/graphql",
        json={"query": query, "variables": variables},
        headers=HEADERS
    )
    resp.raise_for_status()

def main():
    with open(ROADMAP_FILE, "r", encoding="utf-8") as f:
        roadmap = yaml.safe_load(f)
    for issue in roadmap.get("issues", []):
        title = issue["title"]
        body = issue.get("body", "")
        labels = issue.get("labels", [])
        print(f"Creating issue: {title}")
        issue_number, issue_node_id = create_issue(title, body, labels)
        print(f"  Created issue #{issue_number}")
        item_id = add_to_project(issue_node_id)
        print(f"  Added to project as item {item_id}")
        status = issue.get("status", "backlog")
        set_status(item_id, status)
        print(f"  Status set to {status.title()}")

if __name__ == "__main__":
    main()
