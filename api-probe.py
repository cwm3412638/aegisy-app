#!/usr/bin/env python3
"""
Aegisy API 端点探测工具
用于探测 sub2api 的实际 API 接口
"""

import requests
import json
from urllib.parse import urljoin

BASE_URL = "https://www.aegisy.cc"
# 或者使用本地：BASE_URL = "http://localhost"

def probe_endpoints():
    """探测常见的 API 端点"""

    endpoints = [
        # 认证相关
        "/api/v1/auth/login",
        "/api/v1/auth/register",
        "/api/v1/auth/logout",
        "/api/v1/user",
        "/api/v1/user/info",

        # API Keys
        "/api/v1/keys",
        "/api/v1/api-keys",
        "/api/v1/tokens",

        # 账户
        "/api/v1/account",
        "/api/v1/balance",
        "/api/v1/usage",

        # 其他
        "/api/v1/models",
        "/api/v1/channels",
        "/health",
        "/v1/models",
    ]

    print("=" * 60)
    print("Aegisy API 端点探测")
    print("=" * 60)
    print(f"目标：{BASE_URL}\n")

    results = []

    for endpoint in endpoints:
        url = urljoin(BASE_URL, endpoint)
        try:
            # 尝试 GET
            resp_get = requests.get(url, timeout=5, verify=False)
            status_get = resp_get.status_code

            # 尝试 POST
            resp_post = requests.post(url, json={}, timeout=5, verify=False)
            status_post = resp_post.status_code

            result = {
                "endpoint": endpoint,
                "GET": status_get,
                "POST": status_post,
                "exists": status_get < 500 or status_post < 500
            }
            results.append(result)

            if result["exists"]:
                print(f"✓ {endpoint}")
                print(f"  GET: {status_get}, POST: {status_post}")
                if status_get == 200 or status_post == 200:
                    print(f"  响应样例：{resp_get.text[:100] if status_get == 200 else resp_post.text[:100]}")
            else:
                print(f"✗ {endpoint} (不存在)")

        except requests.exceptions.RequestException as e:
            print(f"✗ {endpoint} (连接失败: {e})")
            results.append({
                "endpoint": endpoint,
                "error": str(e)
            })

        print()

    # 保存结果
    with open("api-probe-results.json", "w") as f:
        json.dump(results, f, indent=2, ensure_ascii=False)

    print("=" * 60)
    print("探测完成！结果已保存到 api-probe-results.json")
    print("=" * 60)

if __name__ == "__main__":
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    probe_endpoints()
