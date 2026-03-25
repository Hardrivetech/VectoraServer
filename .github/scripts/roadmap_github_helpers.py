# Helper to get all issues in the repo (open and closed)
import os
import requests

GITHUB_API = "https://api.github.com"
REPO = os.environ.get("GITHUB_REPOSITORY")
TOKEN = os.environ.get("GH_TOKEN")
HEADERS = {
    "Authorization": f"Bearer {TOKEN}",
    "Accept": "application/vnd.github+json"
}

def get_existing_issue_titles():
    url = f"{GITHUB_API}/repos/{REPO}/issues?state=all&per_page=100"
    titles = set()
    page = 1
    while True:
        resp = requests.get(url + f"&page={page}", headers=HEADERS)
        resp.raise_for_status()
        issues = resp.json()
        if not issues:
            break
        for issue in issues:
            titles.add(issue["title"].strip())
        page += 1
    return titles
