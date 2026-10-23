# Security Policy for LimonOS

## Supported Versions

LimonOS is a custom 64-bit monolithic kernel currently in early development. Only the latest code on the `main` branch receives security updates.

## Reporting a Vulnerability

Since LimonOS operates in Ring 0 and includes a custom network stack and TLS 1.3 implementation, security is critical. 

If you find a vulnerability (such as a memory corruption bug, a flaw in the cryptographic library, or a networking issue), **please do not create a public issue**. 

Instead:
1. Go to the **Security** tab at the top of this repository.
2. Click **Advisories** on the left menu.
3. Click **Report a vulnerability**.

I will review your report and work on a fix. Thank you for helping make LimonOS stable and secure!
