By contributing code to the Redis project in any form you agree to the Redis Software Grant and
Contributor License Agreement attached below. Only contributions made under the Redis Software Grant
and Contributor License Agreement may be accepted by Redis, and any contribution is subject to the
terms of the Redis dual-license under RSALv2/SSPLv1 as described in the LICENSE.txt file included in
the Redis source distribution.

# REDIS SOFTWARE GRANT AND CONTRIBUTOR LICENSE AGREEMENT

To specify the intellectual property license granted in any Contribution, Redis Ltd., ("**Redis**")
requires a Software Grant and Contributor License Agreement ("**Agreement**"). This Agreement is for
your protection as a contributor as well as the protection of Redis and its users; it does not
change your rights to use your own Contribution for any other purpose.

By making any Contribution, You accept and agree to the following terms and conditions for the
Contribution. Except for the license granted in this Agreement to Redis and the recipients of the
software distributed by Redis, You reserve all right, title, and interest in and to Your
Contribution.

1. **Definitions**

    1.1. "**You**" (or "**Your**") means the copyright owner or legal entity authorized by the
    copyright owner that is entering into this Agreement with Redis. For legal entities, the entity
    making a Contribution and all other entities that Control, are Controlled by, or are under
    common Control with that entity are considered to be a single contributor. For the purposes of
    this definition, "**Control**" means (i) the power, direct or indirect, to cause the direction
    or management of such entity, whether by contract or otherwise, or (ii) ownership of fifty
    percent (50%) or more of the outstanding shares, or (iii) beneficial ownership of such entity.

    1.2. "**Contribution**" means the code, documentation, or any original work of authorship,
    including any modifications or additions to an existing work described above.

2. "**Work**" means any software project stewarded by Redis.

3. **Grant of Copyright License**. Subject to the terms and conditions of this Agreement, You grant
   to Redis and to the recipients of the software distributed by Redis a perpetual, worldwide,
   non-exclusive, no-charge, royalty-free, irrevocable copyright license to reproduce, prepare
   derivative works of, publicly display, publicly perform, sublicense, and distribute Your
   Contribution and such derivative works.

4. **Grant of Patent License**. Subject to the terms and conditions of this Agreement, You grant to
   Redis and to the recipients of the software distributed by Redis a perpetual, worldwide,
   non-exclusive, no-charge, royalty-free, irrevocable (except as stated in this section) patent
   license to make, have made, use, offer to sell, sell, import, and otherwise transfer the Work,
   where such license applies only to those patent claims licensable by You that are necessarily
   infringed by Your Contribution alone or by a combination of Your Contribution with the Work to
   which such Contribution was submitted. If any entity institutes patent litigation against You or
   any other entity (including a cross-claim or counterclaim in a lawsuit) alleging that your
   Contribution, or the Work to which you have contributed, constitutes a direct or contributory
   patent infringement, then any patent licenses granted to the claimant entity under this Agreement
   for that Contribution or Work terminate as of the date such litigation is filed.

5. **Representations and Warranties**. You represent and warrant that: (i) You are legally entitled
   to grant the above licenses; and (ii) if You are an entity, each employee or agent designated by
   You is authorized to submit the Contribution on behalf of You; and (iii) your Contribution is
   Your original work, and that it will not infringe on any third party's intellectual property
   right(s).

6. **Disclaimer**. You are not expected to provide support for Your Contribution, except to the
   extent You desire to provide support. You may provide support for free, for a fee, or not at all.
   Unless required by applicable law or agreed to in writing, You provide Your Contribution on an
   "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
   including, without limitation, any warranties or conditions of TITLE, NON-INFRINGEMENT,
   MERCHANTABILITY, or FITNESS FOR A PARTICULAR PURPOSE.

7. **Enforceability**. Nothing in this Agreement will be construed as creating any joint venture,
   employment relationship, or partnership between You and Redis. If any provision of this Agreement
   is held to be unenforceable, the remaining provisions of this Agreement will not be affected.
   This represents the entire agreement between You and Redis relating to the Contribution.

# IMPORTANT: HOW TO USE REDIS GITHUB ISSUES

GitHub issues SHOULD ONLY BE USED to report bugs and for DETAILED feature
requests. Everything else should be asked on Discord:
      
    https://discord.com/invite/redis

PLEASE DO NOT POST GENERAL QUESTIONS that are not about bugs or suspected
bugs in the GitHub issues system. We'll be delighted to help you and provide
all the support on Discord.

There is also an active community of Redis users at Stack Overflow:

    https://stackoverflow.com/questions/tagged/redis

Issues and pull requests for documentation belong on the redis-doc repo:

    https://github.com/redis/redis-doc

If you are reporting a security bug or vulnerability, see SECURITY.md.

# How to provide a patch for a new feature

1. If it is a major feature or a semantical change, please don't start coding
straight away: if your feature is not a conceptual fit you'll lose a lot of
time writing the code without any reason. Start by posting in the mailing list
and creating an issue at Github with the description of, exactly, what you want
to accomplish and why. Use cases are important for features to be accepted.
Here you can see if there is consensus about your idea.

2. If in step 1 you get an acknowledgment from the project leaders, use the
   following procedure to submit a patch:

    a. Fork Redis on GitHub ( https://docs.github.com/en/github/getting-started-with-github/fork-a-repo )
    b. Create a topic branch (git checkout -b my_branch)
    c. Push to your branch (git push origin my_branch)
    d. Initiate a pull request on GitHub ( https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/creating-a-pull-request )
    e. Done :)

3. Keep in mind that we are very overloaded, so issues and PRs sometimes wait
for a *very* long time. However this is not a lack of interest, as the project
gets more and more users, we find ourselves in a constant need to prioritize
certain issues/PRs over others. If you think your issue/PR is very important
try to popularize it, have other users commenting and sharing their point of
view, and so forth. This helps.

4. For minor fixes - open a pull request on GitHub.

Additional information on the RSALv2/SSPLv1 dual-license is also found in the LICENSE.txt file.
