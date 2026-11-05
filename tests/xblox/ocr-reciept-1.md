## ocr-reciept-1.png

**Layout:** ok (15 regions)  

| # | label | score | x0 | y0 | x1 | y1 |
|---|-------|-------|-----|-----|-----|-----|
| 2 | paragraph_title | 0.705469 | 19 | 15 | 278 | 39 |
| 8 | text | 0.625725 | 28 | 54 | 151 | 69 |
| 13 | text | 0.626589 | 29 | 73 | 108 | 86 |
| 17 | text | 0.765458 | 28 | 91 | 134 | 105 |
| 22 | text | 0.778901 | 29 | 110 | 107 | 124 |
| 28 | text | 0.72922 | 28 | 130 | 99 | 143 |
| 30 | table | 0.798393 | 292 | 50 | 545 | 109 |
| 46 | table | 0.890503 | 19 | 146 | 272 | 222 |
| 92 | text | 0.695218 | 302 | 130 | 397 | 143 |
| 99 | text | 0.674484 | 302 | 149 | 388 | 163 |
| 101 | text | 0.682092 | 302 | 168 | 382 | 181 |
| 109 | text | 0.733333 | 302 | 187 | 408 | 200 |
| 112 | text | 0.77062 | 302 | 206 | 456 | 220 |
| 131 | table | 0.943948 | 18 | 231 | 546 | 486 |
| 208 | table | 0.864491 | 22 | 560 | 547 | 696 |

**VLM prompt:** `Table Recognition:`  

### OCR text

```
BASIC RECEIPT TEMPLATE
Your Company Name
DATE
08/30/17
123 Main Street
RECEIPT NO.
A246
Hamilton, OH 44416
CUSTOMER NO.
114H
(321) 456-7890
Email Address
ATTN: Name / Dept
SALESPERSON
John Smith
Company Name
CREDIT CARD NO.
0000-0000-0000-0000
123 Main Street
CHECK NO.
Hamilton, OH 44416
MONET ORD. NO.
(321) 456-789C
Email Address
ITEM NO.
DESCRIPTION
QTY
UNIT PRICE
TOTAL
A111
Women's Tall - M
10
10.00
100.00
8222
Men's Tall - M
5
20.00
100.00
C333
Children's - S
10
5.00
50.00
D444
Men's - XL
5
10.00
50.00
0.00
0.00
0.00
0.00
0.00
0.00
Remarks / Instructions:
SUBTOTAL
300.00
TAX (3.8%)
11.40
SHIPPING / HANDLING
30.00
Please make check payable to Your Company Name.
OTHER
15.00
THANK YOU
TOTAL
$
356.40
CASH
CREDIT CARD
MONEY ORDER
For questions concerning this receipt please contact Name, (321) 456-7890, Email Address.
www.yourwebaddress.com
CLIENT
SALES RECEIPT
ATTN: Name / Dept
Company Name
DATE
08/30/17
123 Main Street
RECEIPT NO.
A246
Hamilton, OH 44416
AMOUNT PAID
$
356.40
Your Company Name
123 Main Street
Email Address
(321) 456-7890
Hamilton, OH 44416
www.yourwebaddress.com
```
