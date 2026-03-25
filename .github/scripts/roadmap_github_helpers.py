# Helper to get all issues in the repo (open and closed)
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
