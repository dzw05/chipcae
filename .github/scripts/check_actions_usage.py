#!/usr/bin/env python3
"""Alert when GitHub-hosted Actions minutes cross 80% of the repo budget.

The billing API is not available to GITHUB_TOKEN. This script no-ops unless
ACTIONS_BILLING_TOKEN is present. It does not create that secret.

The figure returned by the API is the account total (the owner), not this
repository alone. The budget in docs/ci.md is this repository's cap inside
that account total.
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request


def _request(url: str, token: str, method: str = "GET", payload: dict | None = None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Authorization", f"Bearer {token}")
    request.add_header("Accept", "application/vnd.github+json")
    request.add_header("X-GitHub-Api-Version", "2022-11-28")
    if payload is not None:
        request.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(request, timeout=30) as response:
        body = response.read().decode("utf-8")
        return response.status, json.loads(body) if body else {}


def billing_minutes(owner: str, token: str) -> dict:
    errors = []
    for kind in ("orgs", "users"):
        url = f"https://api.github.com/{kind}/{owner}/settings/billing/actions"
        try:
            status, body = _request(url, token)
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            errors.append(f"{kind}: HTTP {exc.code} {detail[:300]}")
            if exc.code in {401, 403}:
                print("ACTIONS_BILLING_TOKEN cannot read Actions billing.")
                print("The token needs access to the owner billing API")
                print("(classic PAT scope `user` / `admin:org`, or a fine-grained")
                print("token that can read Actions billing for this owner).")
                print("See docs/ci.md. This workflow does not create the secret.")
                return {}
            continue
        if status == 200 and "total_minutes_used" in body:
            return body
        errors.append(f"{kind}: unexpected payload")
    print("Could not read Actions billing for this owner:")
    for line in errors:
        print(f"  {line}")
    return {}


def _issue_api(repo: str, github_token: str, path: str, method: str = "GET", payload: dict | None = None):
    return _request(f"https://api.github.com/repos/{repo}{path}", github_token, method, payload)


def upsert_issue(repo: str, github_token: str, title: str, body: str) -> None:
    try:
        _status, existing = _issue_api(repo, github_token, "/issues?state=open&per_page=50")
    except urllib.error.HTTPError as exc:
        print(f"could not list issues: HTTP {exc.code}")
        print(body)
        return
    if not isinstance(existing, list):
        print("issues API did not return a list; printed the alert instead.")
        print(body)
        return
    match = next((issue for issue in existing if issue.get("title") == title), None)
    try:
        if match is None:
            _issue_api(
                repo,
                github_token,
                "/issues",
                "POST",
                {"title": title, "body": body},
            )
            print(f"opened issue: {title}")
            return
        number = match["number"]
        _issue_api(repo, github_token, f"/issues/{number}", "PATCH", {"body": body})
        _issue_api(
            repo,
            github_token,
            f"/issues/{number}/comments",
            "POST",
            {"body": body},
        )
        print(f"updated issue #{number}")
    except urllib.error.HTTPError as exc:
        print(f"could not open or update the issue: HTTP {exc.code}")
        print(body)


def main() -> int:
    token = os.environ.get("ACTIONS_BILLING_TOKEN", "")
    budget = float(os.environ.get("ACTIONS_MINUTE_BUDGET", "1500"))
    fraction = float(os.environ.get("ACTIONS_USAGE_ALERT_FRACTION", "0.8"))
    owner = os.environ["GITHUB_REPOSITORY_OWNER"]
    repo = os.environ["GITHUB_REPOSITORY"]
    threshold = budget * fraction
    print(f"budget={budget:g} minutes, alert threshold={threshold:g} ({fraction:.0%})")
    if not token:
        print("ACTIONS_BILLING_TOKEN is not set. Skipping the billing API.")
        print("Hosted-minute alerting stays off until that secret exists.")
        print("See docs/ci.md. No secret was created.")
        return 0
    usage = billing_minutes(owner, token)
    if not usage:
        # A missing scope is a configuration gap, not a code failure.
        return 0
    used = float(usage["total_minutes_used"])
    included = usage.get("included_minutes")
    breakdown = usage.get("minutes_used_breakdown", {})
    print(f"account total_minutes_used={used:g} included_minutes={included}")
    print(f"breakdown={json.dumps(breakdown, sort_keys=True)}")
    if used < threshold:
        print("under the alert threshold.")
        return 0
    title = "[actions-usage] Hosted Actions minutes are past 80% of the chipcae budget"
    body = (
        "This issue is opened by `.github/workflows/actions-usage.yml`.\n\n"
        f"- Account `{owner}` Actions minutes used this billing period: **{used:.0f}**\n"
        f"- chipcae budget: **{budget:.0f}** hosted minutes / month\n"
        f"- Alert threshold: **{threshold:.0f}** ({fraction:.0%} of the budget)\n"
        f"- Included minutes reported by the API: `{included}`\n"
        f"- Breakdown: `{json.dumps(breakdown, sort_keys=True)}`\n\n"
        "The API reports the owner account, not this repository alone. "
        "The budget leaves the rest of the 2,000 included minutes as headroom. "
        "See docs/ci.md before moving more jobs onto GitHub-hosted runners.\n"
    )
    github_token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN", "")
    if not github_token:
        print(body)
        print("GITHUB_TOKEN is missing; printed the alert instead of opening an issue.")
        return 0
    upsert_issue(repo, github_token, title, body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
